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
from owned_graphics_arch import execution_architecture, inspect_pe, macho_architectures


SUITE_PREFIXES = {
    "smoke": "OWNED_D3D12_SMOKE",
    "multiqueue": "OWNED_D3D12_MULTIQUEUE",
    "capabilities": "OWNED_CAPABILITIES",
    "temporal": "OWNED_D3D12_TEMPORAL",
    "ngx": "OWNED_D3D12_NGX",
    "ngx-contracts": "OWNED_D3D12_NGX_CONTRACTS",
    "ngx-features": "OWNED_D3D12_NGX_FEATURES",
    "ngx-alpha": "OWNED_D3D12_NGX_ALPHA",
    "ngx-fg": "OWNED_D3D12_NGX_FG",
    "ngx-fg-present": "OWNED_D3D12_NGX_PRESENT",
    "sr-alpha": "OWNED_D3D12_SR_ALPHA",
    "fsr": "OWNED_D3D12_FSR",
    "fsr-debug": "OWNED_D3D12_FSR_DEBUG",
    "fsr-fg": "OWNED_D3D12_FSR_FG",
    "nvapi-reflex": "OWNED_NVAPI_REFLEX",
    "queue-priority": "OWNED_QUEUE_PRIORITY",
    "corewindow-composition": "OWNED_D3D12_COREWINDOW_COMPOSITION",
    "composition": "OWNED_D3D12_COMPOSITION",
    "diagnostics": "OWNED_D3D12_DIAGNOSTICS",
    "agility": "OWNED_D3D12_AGILITY",
    "shader-cache": "OWNED_D3D12_SHADER_CACHE",
    "core-contracts": "OWNED_D3D12_CORE_CONTRACTS",
    "execution-safety": "OWNED_D3D12_EXECUTION_SAFETY",
    "arena-heap-arena": "OWNED_D3D12_ARENA_HEAP_ARENA",
    "arena-heap-mapping": "OWNED_D3D12_ARENA_HEAP_MAPPING",
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
    "atomic64-phase": "OWNED_D3D12_ATOMIC64_PHASE",
    "atomic64-helpers": "OWNED_D3D12_ATOMIC64_HELPERS",
    "atomic64-pixel-coupled": "OWNED_D3D12_ATOMIC64_PIXEL",
    "atomic64-pixel-contention": "OWNED_D3D12_ATOMIC64_PIXEL_CONTENTION",
    "atomic64-groupshared": "OWNED_D3D12_ATOMIC64",
    "atomic64-groupshared-capacity": "OWNED_D3D12_ATOMIC64",
    "atomic64-typed": "OWNED_D3D12_ATOMIC64_TYPED",
    "present-queues": "OWNED_D3D12_PRESENT_QUEUES",
    "hdr-metadata": "OWNED_D3D12_HDR_METADATA",
    "colorspace": "OWNED_D3D12_COLORSPACE",
    "fullscreen": "OWNED_DXGI_FULLSCREEN",
    "dxgi-fullscreen": "OWNED_DXGI_FULLSCREEN",
    "dxgi-factory": "DXGI_FACTORY_EVENTS",
    "stream-output": "OWNED_D3D12_STREAM_OUTPUT",
    "so-format-support": "OWNED_D3D12_SO_FORMAT_SUPPORT",
    "format-support": "OWNED_D3D12_FORMAT_SUPPORT",
    "expanded-formats": "OWNED_D3D12_EXPANDED_FORMATS",
    "partial-3d-uav-dxil": "OWNED_D3D12_PARTIAL_3D_UAV",
    "packed-cubearray": "OWNED_D3D12_PACKED_CUBE",
    "standard-swizzle": "OWNED_D3D12_STANDARD_SWIZZLE",
    "relaxed-casting": "RELAXED_CASTING",
    "relaxed-casting-shared": "RELAXED_CASTING_SHARED",
    "relaxed-casting-volume": "RELAXED_CASTING_VOLUME",
    "relaxed-casting-volume-standard": "RELAXED_CASTING_VOLUME_STANDARD",
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
    "advanced-texture": "OWNED_D3D12_ADVANCED_TEXTURE",
    "depth-casting": "OWNED_D3D12_DEPTH_CASTING",
    "depth-bounds": "OWNED_D3D12_DEPTH_BOUNDS",
    "dxil-resource-review": "OWNED_D3D12_DXIL_RESOURCE_REVIEW",
    "raytracing": "OWNED_D3D12_RAYTRACING",
    "raytracing-atomic64": "OWNED_D3D12_RAYTRACING_ATOMIC64",
    "raytracing-omm": "OWNED_D3D12_OMM",
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
    "waves": "OWNED_D3D12_WAVES",
    "fp64": "OWNED_D3D12_FP64",
    "vectors": "OWNED_D3D12_VECTORS",
    "matrix-shaders": "OWNED_D3D12_MATRICES",
    "writable-msaa": "OWNED_D3D12_WRITABLE_MSAA",
    # SHARING: shared heaps/resources/fences across devices and processes.
    "sharing": "OWNED_D3D12_SHARING",
    # FL: feature level 11_1/12_0 requirements (logic ops, forced sample counts, TIR).
    "feature-level": "OWNED_D3D12_FEATURE_LEVEL",
    "options13": "OWNED_D3D12_OPTIONS13",
    "conservative-raster": "OWNED_D3D12_CONSERVATIVE_RASTER",
    # VIEWINST: ViewInstancingTier 1 (draw-level looping) with exact pixels.
    "view-instancing": "OWNED_D3D12_VIEW_INSTANCING",
    # MESH: MeshShaderTier 1 (AS/MS pipelines, DispatchMesh, ExecuteIndirect DISPATCH_MESH) with exact pixels.
    "mesh-shader": "OWNED_D3D12_MESH_SHADER",
    "atomic64-mesh-groupshared": "OWNED_D3D12_MESH_SHADER",
    "atomic64-producers": "OWNED_D3D12_ATOMIC64_PRODUCERS",
    # FEEDBACK: SamplerFeedbackTier 1.0 (feedback maps, WriteSamplerFeedback*, transcoding) against a CPU oracle.
    "sampler-feedback": "OWNED_D3D12_SAMPLER_FEEDBACK",
    "work-graphs": "OWNED_D3D12_WORK_GRAPHS",
    "workgraphs-atomic64": "OWNED_D3D12_ATOMIC64_WORKGRAPH",
    "workgraphs-atomic64-typed": "OWNED_D3D12_ATOMIC64_WG_TYPED",
    "matrix-runtime": "MATRIX_RUNTIME",
    "matrix-capabilities": "MATRIX_CAPABILITIES",
    "wave-mma-capabilities": "MATRIX_CAPABILITIES",
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
    "adapter-luid-lifetime-errors",
)
DEFAULT_LOCK = REPOSITORY_ROOT / "graphics-translator/upstream.lock.json"
DEFAULT_DXIL_DIR = REPOSITORY_ROOT / "graphics-translator/tests/dxil"
FG_CAPTURE_CHECKER = REPOSITORY_ROOT / "graphics-translator/tests/check-owned-fg-capture.py"
# NGX and FSR fixtures run on the temporal backend and publish its identity line.
BACKEND_PREFIXES = {
    "raytracing-atomic64": "OWNED_D3D12_RAYTRACING",
    "relaxed-casting-shared": "RELAXED_CASTING",
    "relaxed-casting-volume": "RELAXED_CASTING",
    "relaxed-casting-volume-standard": "RELAXED_CASTING",
    "waves": "OWNED_D3D12_SM6",
    "fp64": "OWNED_D3D12_SM6",
    "arena-heap-arena": "OWNED_D3D12_ARENA_HEAP",
    "arena-heap-mapping": "OWNED_D3D12_ARENA_HEAP",
    "ngx": "OWNED_D3D12_TEMPORAL",
    "ngx-contracts": "OWNED_D3D12_TEMPORAL",
    "ngx-features": "OWNED_D3D12_TEMPORAL",
    "ngx-alpha": "OWNED_D3D12_TEMPORAL",
    "ngx-fg": "OWNED_D3D12_TEMPORAL",
    "ngx-fg-present": "OWNED_D3D12_TEMPORAL",
    "sr-alpha": "OWNED_D3D12_TEMPORAL",
    "fsr-debug": "OWNED_D3D12_TEMPORAL",
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
DXIL_TEXTURE_SHADERS = ("basic-texture-vs.dxil", "basic-texture-ps.dxil",
                        "programmable-offsets-vs.dxil", "programmable-offsets-ps.dxil",
                        "programmable-offsets-cs.dxil")
ADVANCED_TEXTURE_SHADERS = (
    tuple(f"advanced-texture-cmp{shape}-{stage}.dxil"
          for shape in range(6) for stage in ("cs", "vs", "ps", "gs", "hs", "ds", "as", "ms", "lib"))
    + tuple(f"advanced-texture-raw{bits}-{shape}.dxil" for bits in (16, 32, 64) for shape in (2, 3))
    + ("advanced-texture-null-vs.dxil", "advanced-texture-null-ps.dxil"))
ADVANCED_TEXTURE_CASES = (
    tuple(f"cmp{shape}_{stage}_shape{shape}" for shape in range(6) for stage in ("cs", "vs", "ps"))
    + tuple(f"raw{bits}_cs_shape{shape}" for bits in (16, 32, 64) for shape in (2, 3)))
DEPTH_CASTING_CASES = ("d16_upload", "d16_clear", "d32_upload", "d32_clear",
                      "d16_msaa_draw", "d16_msaa_clear", "d32_msaa_draw", "d32_msaa_clear")
DEPTH_BOUNDS_CASES = (
    "stored_not_incoming", "incoming_inside_stored_outside", "odepth_not_stored",
    "inclusive_min", "inclusive_max", "inverted", "inverted_above_one", "inverted_below_zero",
    "inverted_late_uav", "inverted_early_uav", "nan_min_zero", "nan_max_zero", "nan_max_reject",
    "default_range", "reset_defaults", "clearstate_defaults", "pso_disabled", "no_dsv",
    "depth_disabled", "depth_disabled_pass", "no_ps", "no_ps_fail", "readonly_pass", "readonly_fail", "readonly_stencil",
    "early_uav_fail", "early_uav_pass", "early_discard_commits_ds", "late_discard_preserves_ds", "late_uav_fail", "bundle_inherit", "bundle_persist",
    "indexed", "indirect", "replay", "array_mip", "desc2_enabled", "desc2_disabled",
    "live_primitive_depth", "d16_updates", "d16_no_updates", "d24_updates", "d24_no_updates",
    "sample4_late_mixed", "sample4_late_fail", "sample4_early_mixed", "sample4_early_fail",
    "sample4_live_primitive_depth", "sample4_d16_updates", "sample4_d24_no_updates",
)
DEPTH_BOUNDS_INPUTS = ("depth-bounds.hlsl", "build-depth-bounds.sh") + tuple(
    f"depth-bounds-{entry}.dxil" for entry in ("vs", "plain", "seed", "late", "early", "lateDiscard", "earlyDiscard", "read", "read-msaa"))
DXIL_RESOURCE_REVIEW_SHADERS = tuple(
    f"resource-review-{entry}.dxil" for entry in ("cs_bounds", "cs_gradients", "cs_array_atomics", "cs_bounds64"))
PREDICATION_INDIRECT_SHADERS = ("predication-indirect-cs.dxil", "predication-indirect-vs.dxil")
ATOMIC_COPY_SHADERS = ("atomic-copy-reader.dxil",)
ATOMIC64_PHASE_INPUTS = ("atomic64-phase.dxil", "atomic64-phase-prepare.dxil",
                        "atomic64-phase-returning.dxil", "atomic64-phase-observe32.dxil",
                        "atomic64-phase.hlsl", "build-atomic64-phase.sh")
STREAM_OUTPUT_SHADERS = ("stream-output-packed-vs.dxil",)
# GS multi-stream cases load these by name beside the fixture (not passed on the command line).
STREAM_OUTPUT_GS_INPUTS = ("stream-output-gs-vs_gs.dxil", "stream-output-gs-gs_multi.dxil")
PARTIAL_3D_UAV_SHADERS = ("partial-3d-uav.dxil",)
# SM: frozen DXIL read by name (sm6-<entry>.dxil) beside the executable; the HLSL and
# its build script are frozen with them so the evidence names the exact sources.
SM6_SOURCES = ("sm6.hlsl", "sm6-16bit.hlsl", "sm6-heap.hlsl", "sm6-reject.hlsl", "sm6-denorm.hlsl",
               "fp64.hlsl", "fp64-legacy.hlsl", "build-sm6.sh", "build-fp64-legacy.sh")
FP64_ENTRIES = ("cs_fp64", "cs_fp64_ftz", "cs_fp64_wave", "vs_fp64", "ps_fp64")
FP64_LEGACY_INPUTS = tuple(f"sm6-{stage}_fp64_legacy{version}.dxbc"
                          for version in (50, 51) for stage in ("cs", "vs", "ps"))
FP64_SOURCES = ("fp64.hlsl", "fp64-legacy.hlsl", "build-sm6.sh", "build-fp64-legacy.sh")
FP64_CASES = ("fp64", "fp64_ftz", "fp64_legacy50", "fp64_legacy51", "fp64_wave",
              "fp64_graphics", "fp64_graphics_legacy50", "fp64_graphics_legacy51")
WRITABLE_MSAA_ENTRIES = tuple(
    f"{entry}{samples}{shape}" for samples in (2, 4, 8) for shape in ("single", "array")
    for entry in ("cs_msaa_store", "cs_msaa_read", "cs_msaa_heap", "cs_msaa_srv", "cs_msaa_oob")
) + ("vs_msaa", "ps_msaa") + tuple(
    f"{entry}{scalar}" for scalar in ("sint", "float")
    for entry in ("cs_msaa_store", "cs_msaa_read", "cs_msaa_srv", "ps_msaa")
) + ("cs_msaa_storeraw16", "cs_msaa_readraw16")
WRITABLE_MSAA_CASES = tuple(
    case for samples in (2, 4, 8) for array in (False, True)
    for case in (("same_dispatch", "next_dispatch_dimensions", "SRV_after_transition", "direct_heap",
                  "out_of_bounds_sample_xy_layer",
                  "UAV_clear_barrier", "native_copy", "native_raster", "native_resolve_min_max")
                 + (("array_subview",) if array else ()) + ("null_store_load",))
) + tuple(f"{scalar}_{case}" for scalar in ("sint", "float")
          for case in ("same_dispatch", "next_dispatch_dimensions", "SRV_after_transition",
                       "native_copy", "native_raster", "native_resolve_min_max", "UAV_clear")) + (
    "typeless16_same_dispatch", "typeless16_next_dispatch", "typeless16_native_copy_float_srv",
)
SM6_ENTRIES = (
    "cs_wave4", "cs_wave8", "cs_wave16", "cs_divergent4", "cs_divergent8", "cs_divergent16",
    "cs_int64wave4", "cs_int64wave8", "cs_int64wave16",
    "cs_wave_range4", "cs_wave_range8", "cs_wave_range16", "cs_wave_range32", "cs_wave_range64",
    "cs_wave_range16max", "cs_wave_sm68fixed4", "cs_wavesize128", "cs_wavesize_range64only",
    "cs_wave", "cs_divergent", "cs_int64", "cs_atomic64", "cs_atomic64_ops", "cs_atomic64_heap", "cs_minprec",
    "cs_quad_deriv", "cs_lod", "cs_lod_shapes", "vs_main", "ps_main", "ps_helper", "ps_helper_all", "vs_tiny",
    "vs_int64", "ps_int64", "vs_wave", "ps_null", "cs_16bit", "cs_packed", "vs_16bit", "ps_16bit", "cs_wavesize32", "cs_wavesize64",
    "cs_fp64", "cs_fp64_ftz", "cs_fp64_wave", "vs_fp64", "ps_fp64", "cs_gs64", "cs_sm69", "vs_viewid", "ps_viewid",
    "ps_viewid_vi", "cs_denorm_any", "cs_denorm_ftz", "cs_denorm_preserve",
)
SM6_CASES = (
    "wave", "wave4", "wave8", "wave16", "wave_range4", "wave_range8", "wave_range16", "wave_range32",
    "wave_range64_unavailable", "wave_range16max", "wave_sm68fixed4",
    "divergent", "divergent4", "divergent8", "divergent16",
    "int64", "int64wave4", "int64wave8", "int64wave16",
    "atomic64_ops", "atomic64_heap", "16bit", "minprec", "packed", "quad_deriv",
    "calculate_lod", "calculate_lod_shapes",
    "rejections", "groupshared64", "sm69_input", "wavesize32", "barycentrics", "barycentrics_indexed", "helper_lanes", "helper_lanes_included",
    "16bit_io", "graphics_int64", "vs_wave", "sv_viewid", "sv_viewid_instanced",
    "denorm_any", "denorm_ftz", "denorm_preserve",
) + FP64_CASES + (
    "cs_atomic64_stress_1", "cs_atomic64_stress_16",
)
SM6_REJECTS = ["cs_wavesize64", "cs_wavesize128", "cs_wavesize_range64only"]
SM6_STRESS_REPS = 20
WAVE_ENTRIES = tuple(entry for entry in SM6_ENTRIES
                     if entry.startswith(("cs_wave", "cs_divergent", "cs_int64"))
                     or entry in ("cs_quad_deriv", "vs_tiny", "ps_helper", "ps_helper_all", "vs_wave", "ps_null"))
WAVE_CASES = SM6_CASES[:SM6_CASES.index("atomic64_ops")] + (
    "wave_requirements", "quad_deriv", "helper_lanes", "helper_lanes_included", "vs_wave")
VECTOR_ENTRIES = ("cs_vectors", "cs_vectors1024", "cs_vector_denorm", "cs_vector_double", "cs_vector_uint64",
                  "cs_vector_half", "cs_vector_heap", "cs_vector_structured", "vs_vectors", "ps_vectors", "cs_vector_tiles", "cs_vector_ssa")
VECTOR_SOURCES = ("sm6-vectors.hlsl", "build-vectors.sh", "sm6-VECTOR_ROOT.dxbc", "sm6-VECTOR_HEAP_ROOT.dxbc", "compile-vectors.mm", "vector-contract.cpp")
MATRIX_ENTRIES = ("cs_wavematrix", "cs_matrix_wave", "cs_matrix_group", "cs_matrix_required", "cs_matrix_vector", "cs_matrix_atomic", "cs_matrix_formats", "cs_matrix_groupformats", "cs_wavematrix16x64", "cs_wavematrix64x16", "cs_wavematrix64x64")
MATRIX_SOURCES = ("wavematrix.hlsl", "matrix.hlsl", "build-matrix.sh", "matrix-contract.cpp", "matrix-producers.json")
VECTOR_CASES = (("vectors",) * 5 + ("vectors1024",) * 4 + ("vector_denorm",) + ("vector_ssa",) * 8 + ("vector_double", "vector_uint64", "vector_half")
                + ("vector_heap",) * 3 + ("vector_structured",) * 3 + ("vector_graphics",) + ("vector_tiles",) * 4)
# MESH: frozen DXIL read by name (mesh-shader-<entry>.dxil) beside the executable, with its sources.
MESH_SHADER_SOURCES = ("mesh-shader.hlsl", "mesh-shader-contract.hlsl", "build-mesh-shader.sh")
MESH_SHADER_ENTRIES = (
    "ms_groupshared64", "as_groupshared64",
    "ms_derivatives_1d", "ms_derivatives_2d", "ms_derivatives_3d",
    "as_derivatives_1d", "as_derivatives_2d", "as_derivatives_3d",
    "ms_derivative_payload", "ms_derivatives_invalid", "as_derivatives_invalid",
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
    "derivatives_ms_1d", "derivatives_ms_2d", "derivatives_ms_3d",
    "derivatives_as_1d", "derivatives_as_2d", "derivatives_as_3d", "derivatives_invalid_shape",
    "groupshared64_ms", "groupshared64_as",
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
TILED_INPUTS = ("tiled.hlsl", "tiled-fxc.hlsl", "tiled-volume.hlsl")
TILED_SHADERS = ("tiled-write.dxil", "tiled-buffer-status.dxil", "tiled-texture-status.dxil",
                 "tiled-dxbc-status.dxbc", "tiled-fxc-cs.dxbc", "tiled-fxc-vs.dxbc", "tiled-fxc-ps.dxbc",
                 "tiled-root-status.dxil", "tiled-rgb32-status.dxil", "tiled-array-status.dxil",
                 "tiled-volume-read.dxil", "tiled-volume-uint.dxil", "tiled-volume-vs.dxil",
                 "tiled-volume-ps.dxil", "tiled-volume-color.dxil", "tiled-volume-read.dxbc",
                 "tiled-volume-uint.dxbc", "tiled-volume-vs.dxbc", "tiled-volume-ps.dxbc",
                 "tiled-volume-color.dxbc")
TILED_CASES = (
    "buffer-tiling", "buffer-map-two-heaps", "buffer-null-tile", "copy-tile-mappings",
    "buffer-residency-status", "copy-tiles-buffer-to-linear", "mapping-update-at-execute-boundary",
    "invalid-update-dropped", "evict-make-resident", "reuse-single-tile", "cross-queue-mapping-order",
    "texture-tiling", "texture-null-tile", "texture-packed-mips", "texture-residency-status",
    "texture-remap-at-execute-boundary", "min-max-reduction", "dxbc-feedback-status", "fxc-feedback-status",
    "root-view-and-rgb32-status", "copy-tiles-round-trip",
    "copy-tiles-unmapped-reads-zero", "array-full-chain-independent-tail-readback",
    "array-null-tail-point-linear-residency", "array-tail-alias-crossqueue-order",
    "array-null-tail-writes-discarded-and-remap-retains-data", "array-tail-alias-write-visible-through-both-slices",
    "array-packed-copy-undefined-for-every-slice", "copy-tiles-invalid", "reserved-creation-validation",
    "volume-dxil-filter-lod-address-residency", "volume-dxbc-filter-lod-address-residency",
    "volume-dxil-implicit-bias-calculateLOD", "volume-dxbc-implicit-bias-calculateLOD",
    "volume-dxil-uint-border-store-atomic-partialW", "volume-dxbc-uint-load-store-atomic-partialW",
    "volume-dxil-dxbc-partialW-OOB-read-write-atomic-preserves-64KB",
    "volume-UAV-UINT-clear-partial-W-null-alias-64KB", "volume-UAV-float-clear-cross-Z-null-alias-64KB",
    "volume-dxil-BC1-sRGB-independent-decode-xyz-mips-null-alias",
    "volume-dxbc-BC1-sRGB-independent-decode-xyz-mips-null-alias",
    "volume-dxil-native-parent-RG32-RGBA16-same-dispatch-partialW-guards",
    "volume-dxbc-native-parent-RG32-RGBA16-same-dispatch-partialW-guards",
    "volume-dxil-BC1-RG32-RGBA16-same-dispatch-xyz-null-alias-mip1-partialW-guards-native-decode",
    "volume-dxbc-BC1-RG32-RGBA16-same-dispatch-xyz-null-alias-mip1-partialW-guards-native-decode",
    "volume-BC1-RG32-RGBA16-dual-UAV-same-dispatch-exact-64KB-NULL-remap",
    "volume-dxil-dxbc-BC1-plain-RG32-load-store-partialW-all-mips",
    "volume-BC1-plain-RG32-UAV-clear-mip2-partialW",
    "volume-dxil-BC1-decode-after-plain-store-clear-same-64KB",
    "volume-dxbc-BC1-decode-after-plain-store-clear-same-64KB",
    "volume-BC1-plain-cast-exact-64KB-alias-byte-oracle",
    "volume-dxil-BC4-UNORM-SNORM-independent-decode-xyz-mips-null-alias",
    "volume-dxbc-BC4-UNORM-SNORM-independent-decode-xyz-mips-null-alias",
    "volume-BC4-signed-cast-identical-64KB",
    "volume-dxil-BC5-UNORM-SNORM-independent-decode-xyz-mips-null-alias",
    "volume-dxbc-BC5-UNORM-SNORM-independent-decode-xyz-mips-null-alias",
    "volume-dxil-BC5-plain16-RGBA32-load-store-xyz-null-alias-all-mips-partialW-guards-native-decode",
    "volume-dxbc-BC5-plain16-RGBA32-load-store-xyz-null-alias-all-mips-partialW-guards-native-decode",
    "volume-BC5-plain16-RGBA32-store-exact-64KB-NULL-remap-alias-guards",
    "volume-dxil-BC5-UNORM-SNORM-decode-after-plain16-store-clear",
    "volume-dxbc-BC5-UNORM-SNORM-decode-after-plain16-store-clear",
    "volume-BC5-plain16-RGBA32-UAV-clear-mip2-partialW-exact-64KB",
    "volume-BC5-signed-cast-identical-64KB",
    "volume-dxil-BC6H-UF16-SF16-nonzero-mode11-independent-decode-xyz-mips-null-alias",
    "volume-dxbc-BC6H-UF16-SF16-nonzero-mode11-independent-decode-xyz-mips-null-alias",
    "volume-BC6H-signed-cast-identical-64KB",
    "volume-copyregion-three-axis-boundary-null-alias",
    "volume-copyregion-RTV-clear-draw-64KB-alias-byte-oracle",
    "volume-xyz-box-linear-mip-SKIP-REUSE-CopyTileMappings-cross-queue",
    "volume-NULL-write-remap-preserves-physical-64KB",
    "volume-predicated-CopyResource-skip-all-mips",
    "volume-predicated-CopyResource-execute-all-mips-64KB-padding",
    "volume-predicated-CopyTextureRegion-skip-xyz", "volume-predicated-CopyTextureRegion-execute-xyz",
    "volume-mip1-partialW-RTV-clear-dxbc-dxil-draw-64KB-alias", "caps",
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
CONSERVATIVE_RASTER_INPUTS += tuple(
    f"conservative-raster-{entry}.{format}" for format in ("dxil", "dxbc")
    for entry in ("ps_inner", "ps_inner_early", "ps_inner_late", "ps_helper")) + (
    "conservative-raster-ps_main.dxbc", "conservative-raster-cs_stencil.dxil",
    "conservative-raster-provenance.json")
# VIEWINST: frozen by dxil/build-view-instancing.sh; the fixture reads view-instancing-<entry>.dxil by name.
VIEW_INSTANCING_INPUTS = ("view-instancing.hlsl", "view-instancing-indices.hlsl", "build-view-instancing.sh") + tuple(
    f"view-instancing-{entry}.dxil"
    for entry in ("vs_main", "ps_uint", "ps_unorm", "gs_view", "vs_tess", "hs_view", "ds_view", "vs_indices"))
VIEW_INSTANCING_CASES = (
    "rtai_2_views", "rtai_4_views", "rtai_out_of_range_to_0", "vpai_2_views", "vpai_4_views",
    "vpai_out_of_range_to_0", "mask_flag_0b1010", "mask_flag_upper_bits_ignored", "mask_flag_zero_draws_nothing",
    "mask_without_flag_ignored", "gs_rtai_2_views", "tess_rtai_2_views",
    "no_subobject_reads_zero_after_instanced_draw", "count0_reads_zero", "count1_location_slice2",
    "msaa4x_rtai_3_views_out_of_range_to_0", "msaa4x_vpai_3_views_out_of_range_to_0", "msaa4x_mask_0b10",
    "execute_indirect_2_views_2_commands", "execute_indirect_masked_rtai_then_direct",
    "execute_indirect_masked_vpai_out_of_range", "execute_indirect_adds_authored_indices_and_clamps",
    "execute_indirect_viewport_uint32_wrap", "execute_indirect_viewport_uint32_valid",
    "execute_indirect_viewport_uint32_oob", "execute_indirect_layer_uint32_wrap",
    "execute_indirect_layer_uint32_valid", "execute_indirect_layer_uint32_oob",
    "bundle_mask_reset_and_leak", "bundle_starts_with_mask_0",
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
                      "raytracing-linkage-final.dxil", "raytracing-hitobject.dxil")
RAYTRACING_OMM_SHADERS = ("raytracing-contract.dxil", "raytracing-omm.dxil", "raytracing-omm-legacy.dxil")
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
    "vertex-formats", "geometry-transform-row-major", "geometry-rules", "transforms", "instances-hit-groups",
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
                   "bindless-rov-vs.dxil", "bindless-rov-ps.dxil",
                   "bindless-nonnormalized.hlsl", "bindless-nonnormalized-cs.dxil",
                   "bindless-nonnormalized-vs.dxil", "bindless-nonnormalized-ps.dxil",
                   "bindless-nonnormalized-raw.hlsl", "bindless-nonnormalized-raw.dxil",
                   "bindless-uint-border.hlsl", "bindless-uint-border-table.dxil", "bindless-uint-border-heap.dxil")
BINDLESS_CASES = (
    "caps", "root-flags-validation", "root-signature-1.2", "uint-border", "cs66-dynamic-resources",
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


ATOMIC64_PRODUCER_COUNTS = {
    "vs": (6, 0, 0, 0, 0, 0), "hs": (0, 6, 0, 0, 0, 0), "ds": (0, 0, 6, 0, 0, 0),
    "gs": (0, 0, 0, 4, 0, 0), "ds_gs": (0, 0, 6, 4, 0, 0), "ms_grid": (0, 0, 0, 0, 0, 16),
    "as_child1": (0, 0, 0, 0, 8, 8), "as_child2": (0, 0, 0, 0, 8, 16),
    "as_child3d": (0, 0, 0, 0, 8, 32),
}
ATOMIC64_PRODUCER_ENTRIES = ("vs_plain", "vs_atomic", "hs_plain", "hs_atomic", "ds_plain", "ds_atomic",
                            "gs_atomic", "as_atomic", "ms_atomic", "ms_payload", "ps_sample")


def atomic64_typed_failures(stdout: str, stderr: str) -> list[str]:
    """Require the complete public Typed64 CPU-oracle matrix, never the first-case diagnostic."""
    failures: list[str] = []
    prefix = "OWNED_D3D12_ATOMIC64_TYPED"
    caps = re.findall(rf"^{prefix}_CAP typed=([01])$", stdout, re.MULTILINE)
    if caps != ["1"] or stdout.count(prefix + "_CAP ") != 1:
        failures.append("Typed64 advertised capability missing, false or duplicated")
    expected_rows = [(str(shape), str(sign)) for shape in range(6) for sign in range(2)]
    rows = re.findall(rf"^{prefix}_CASE shape=(\d+) signed=(\d+)$", stdout, re.MULTILINE)
    begins = re.findall(rf"^{prefix}_BEGIN shape=(\d+) signed=(\d+) first=0$", stdout, re.MULTILINE)
    if rows != expected_rows or begins != expected_rows or stdout.count(prefix + "_CASE ") != 12:
        failures.append("Typed64 requires all 12 ordered shape/sign operation and contention rows")
    # Match the fixture format matrix: every optional format must either execute
    # its independent oracle or explicitly lack the reported typed load/store bits.
    formats = ((65, 1), (61, 1), (63, 1), (62, 1), (64, 1),
               (56, 2), (58, 2), (57, 2), (59, 2), (54, 2),
               (49, 2), (51, 2), (50, 2), (52, 2), (42, 4), (43, 4), (41, 4),
               (35, 4), (37, 4), (36, 4), (38, 4), (34, 4),
               (28, 4), (31, 4), (30, 4), (32, 4), (24, 4), (25, 4), (26, 4),
               (17, 8), (18, 8), (16, 8), (11, 8), (13, 8), (12, 8), (14, 8), (10, 8),
               (3, 16), (4, 16), (2, 16))
    format_rows = re.findall(rf"^{prefix}_FORMAT(_UNSUPPORTED)? format=(\d+) (?:unit|support2)=(\d+)$",
                             stdout, re.MULTILINE)
    if [int(format_id) for _, format_id, _ in format_rows] != [format_id for format_id, _ in formats]:
        failures.append("Typed64 format execution/unsupported coverage is incomplete or duplicated")
    else:
        for (unsupported, format_id, value), (_, unit) in zip(format_rows, formats):
            if unsupported:
                if int(format_id) in (17, 41, 42, 43) or int(value) & 0xc0 == 0xc0:
                    failures.append(f"Typed64 required/reported-supported format {format_id} was skipped")
            elif int(value) != unit:
                failures.append(f"Typed64 format {format_id} storage unit wrong")
    expected_cases = []
    for shape in range(6):
        for sign in range(2):
            expected_cases.extend(("atomic64_typed_operations", "atomic64_typed_contention"))
            if shape == sign == 0:
                expected_cases.append("atomic64_typed_raw32_aliases")
    expected_cases.extend("atomic64_typed_format" for unsupported, _, _ in format_rows if not unsupported)
    expected_cases.append("atomic64_typed_float32_aliases")
    cases = re.findall(r"^OWNED_D3D12_SM6_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
    if cases != expected_cases:
        failures.append("Typed64 operation/contention/raw32/format/float32 CPU-oracle cases missing")
    if stdout.splitlines().count(prefix + "_PASS shapes=6 signs=2 failures=0") != 1:
        failures.append("Typed64 full terminal verdict missing")
    if re.search(r"OWNED_D3D12_SM6_(?:MISMATCH|CASE_FAIL|FAIL)|OWNED_D3D12_ATOMIC64_TYPED_(?:FAIL|FIRST_RESULT)",
                 stdout + stderr):
        failures.append("Typed64 CPU oracle failure or first-case-only diagnostic")
    return failures


def atomic64_producers_failures(stdout: str, stderr: str) -> list[str]:
    """Check actual RMW/CAS words, independent counts, untouched cells and additive primitive pixels."""
    failures: list[str] = []
    cases = [(name, samples) for name in ATOMIC64_PRODUCER_COUNTS for samples in (1, 4)]
    rows = re.findall(r"^OWNED_D3D12_ATOMIC64_PRODUCERS_RECORD case=(\w+) samples=(\d+) "
                      r"stage=(\d+) lane=(\d+) old=([0-9a-f]{16}) cas=([0-9a-f]{16}) count=(\d+) "
                      r"cell=([0-9a-f]{16}) guard=([0-9a-f]{8}),([0-9a-f]{8}),([0-9a-f]{8})$", stdout, re.MULTILINE)
    expected_keys = [(name, str(samples), str(stage), str(lane)) for name, samples in cases
                     for stage in range(6) for lane in range(64)]
    if ([row[:4] for row in rows] != expected_keys
            or stdout.count("OWNED_D3D12_ATOMIC64_PRODUCERS_RECORD ") != len(expected_keys)):
        failures.append("atomic64 producers require every ordered record exactly once")
    else:
        for name, samples, stage, lane, old, cas, count, cell, *guards in rows:
            stage, lane = int(stage), int(lane)
            initial = 0x1234567800000000 + (stage * 64 + lane) * 256
            active = lane < ATOMIC64_PRODUCER_COUNTS[name][stage]
            expected = (initial, initial + 1, 1, initial + 9) if active else (0xcdcdcdcdcdcdcdcd,) * 2 + (0, initial)
            if ((int(old, 16), int(cas, 16), int(count), int(cell, 16)) != expected
                    or guards != ["cdcdcdcd"] * 3):
                failures.append(f"atomic64 producer {name}/{samples}/{stage}/{lane} readback mismatch")
    pixels = re.findall(r"^OWNED_D3D12_ATOMIC64_PRODUCERS_PIXEL case=(\w+) samples=(\d+) "
                        r"index=(\d+) rgba=([0-9a-f]{8})$", stdout, re.MULTILINE)
    if ([(name, sample, index) for name, sample, index, _ in pixels] !=
            [(name, str(samples), str(index)) for name, samples in cases for index in range(64)]
            or stdout.count("OWNED_D3D12_ATOMIC64_PRODUCERS_PIXEL ") != len(cases) * 64):
        failures.append("atomic64 producers require every ordered primitive-count pixel")
    else:
        for name, sample, index, rgba in pixels:
            counts = ATOMIC64_PRODUCER_COUNTS[name]
            primitives = counts[5] // 4 if counts[5] else 4 if counts[3] else 2
            expected = (primitives * 16 << 24) | (primitives * (10 if sample == "4" else 4) << 8)
            if int(rgba, 16) != expected:
                failures.append(f"atomic64 producer {name}/{sample} pixel {index} output/count mismatch")
    verdicts = re.findall(r"^OWNED_D3D12_ATOMIC64_PRODUCERS_CASE case=(\w+) samples=(\d+) status=(PASS|FAIL)$",
                          stdout, re.MULTILINE)
    if (verdicts != [(name, str(samples), "PASS") for name, samples in cases]
            or stdout.count("OWNED_D3D12_ATOMIC64_PRODUCERS_CASE ") != len(cases)
            or stdout.splitlines().count("OWNED_D3D12_ATOMIC64_PRODUCERS_PASS cases=18 stages=VS,HS,DS,GS,AS,MS") != 1):
        failures.append("atomic64 producer completion missing, duplicated or failed")
    if re.search(r"OWNED_D3D12_ATOMIC64_PRODUCERS_FAIL|OWNED_D3D12_SM6_MISMATCH|kIOGPUCommandBufferCallbackError", stdout + stderr):
        failures.append("atomic64 producer fixture or GPU failure")
    return failures


def workgraphs_atomic64_typed_failures(stdout: str, stderr: str) -> list[str]:
    """Check all returning values and physical API-copy texels of the non-aliasing typed publication consumer."""
    failures: list[str] = []
    rows = re.findall(r"^OWNED_D3D12_ATOMIC64_WG_TYPED_TEXEL index=(\d+) old=0x([0-9a-f]{16}) "
                      r"final=0x([0-9a-f]{16}) copy=0x([0-9a-f]{16})$", stdout, re.MULTILINE)
    if ([int(row[0]) for row in rows] != list(range(16))
            or stdout.count("OWNED_D3D12_ATOMIC64_WG_TYPED_TEXEL ") != 16):
        failures.append("typed WG publisher requires all16 ordered actual physical-copy texels")
    else:
        for index, old, final, copied in rows:
            initial = (0xfffffffafffff000 + int(index) * 0x100000001) % (1 << 64)
            expected = (initial + 0x40000000f) % (1 << 64)
            if (int(old, 16), int(final, 16), int(copied, 16)) != (initial, expected, expected):
                failures.append(f"typed WG texel{index} returning old/final or physical publication wrong")
    required = ("OWNED_D3D12_ATOMIC64_WG_TYPED_CAP library_profile=lib_6_8 typed_cap=1",
                "OWNED_D3D12_ATOMIC64_WG_TYPED_COUNTS count=16 guard=a11c0ded",
                "OWNED_D3D12_ATOMIC64_WG_TYPED_PASS texels=16 full64_words=48 nonaliasing_views=1 API_barrier=1 physical_copy=PASS count=16 guard=PASS")
    if any(stdout.splitlines().count(marker) != 1 for marker in required):
        failures.append("typed WG publisher exact capability/completion/count/guard missing or duplicated")
    if re.search(r"OWNED_D3D12_ATOMIC64_WG_TYPED_FAIL|kIOGPUCommandBufferCallbackError", stdout + stderr):
        failures.append("typed WG fixture or GPU failure")
    return failures


def workgraphs_atomic64_failures(stdout: str, stderr: str) -> list[str]:
    """Independently check every returned 64-bit input/output/descendant value, not fixture PASS text."""
    failures: list[str] = []
    rows = re.findall(r"^OWNED_D3D12_ATOMIC64_WORKGRAPH_RECORD mode=(CPU|GPU) index=(\d+) words=\("
                      + ",".join([r"0x([0-9a-f]{16})"] * 12) + r"\)$", stdout, re.MULTILINE)
    indices = [(mode, str(index)) for mode in ("CPU", "GPU") for index in range(16)]
    if ([row[:2] for row in rows] != indices
            or stdout.count("OWNED_D3D12_ATOMIC64_WORKGRAPH_RECORD ") != 32):
        failures.append("record64 requires both complete ordered CPU/GPU mode readbacks")
    else:
        mask64 = (1 << 64) - 1
        for mode, index, *values in rows:
            i = int(index)
            initial = (0xfffffffafffff000 + i * 0x100000001) & mask64
            added = (initial + 0x40000000f) & mask64
            before_xor = added ^ 0xa5c35a3cf00d1234
            after_xor = before_xor ^ 0x13579bdf2468ace0
            after_cas = (after_xor + 0x40000000f) & mask64
            guard = 0xf00d1234 ^ i
            guard_new = guard + 3
            expected = (initial, added, before_xor, after_xor, after_xor, after_cas,
                        guard | (guard_new << 32), i, after_cas ^ 0x55aa55aaaa55aa55,
                        0x8000000000000005 + i, i | (guard_new << 32), 0xdeadbeefcafef00d)
            if tuple(int(value, 16) for value in values) != expected:
                failures.append(f"record64 {mode} record{i} full64 old/final/guard alias readback wrong")
    counts = re.findall(r"^OWNED_D3D12_ATOMIC64_WORKGRAPH_COUNTS mode=(CPU|GPU) seed=(\d+) "
                        r"child=(\d+) terminal=(\d+) guard=([0-9a-f]{8})$", stdout, re.MULTILINE)
    if (counts != [(mode, "16", "16", "16", "a11c0ded") for mode in ("CPU", "GPU")]
            or stdout.count("OWNED_D3D12_ATOMIC64_WORKGRAPH_COUNTS ") != 2):
        failures.append("record64 requires exact queue counts and adjacent guard in both modes")
    completed = re.findall(r"^OWNED_D3D12_ATOMIC64_WORKGRAPH_PASS mode=(CPU|GPU) records=16 fields=192 "
                           r"seed_count=16 child_count=16 terminal_count=16 input_bytes=16 input_align=8 "
                           r"queue_guard=PASS old_returns=PASS final_values=PASS$", stdout, re.MULTILINE)
    if (completed != ["CPU", "GPU"] or stdout.splitlines().count(
            "OWNED_D3D12_ATOMIC64_WORKGRAPH_TOTAL_PASS modes=2 fields=384") != 1):
        failures.append("record64 completed GPU consumer evidence missing or duplicated")
    caps = re.findall(r"^OWNED_D3D12_ATOMIC64_WORKGRAPH_CAP library_profile=lib_6_8 "
                      r"advertised_shader_model=[0-9a-f]+ typed_cap=1 heap_cap=1$", stdout, re.MULTILINE)
    if len(caps) != 1:
        failures.append("record64 advertised atomic64 capability missing or duplicated")
    if re.search(r"RECORD64_MISMATCH|OWNED_D3D12_ATOMIC64_WORKGRAPH_FAIL|kIOGPUCommandBufferCallbackError", stdout + stderr):
        failures.append("record64 fixture or GPU failure")
    return failures


def raytracing_atomic64_failures(stdout: str, stderr: str) -> list[str]:
    """Recompute all512 actual RMW/CAS readback words independently of the fixture verdict."""
    failures: list[str] = []
    cases = re.findall(r"^OWNED_D3D12_RAYTRACING_CASE atomic64-rt status=(PASS|FAIL)$", stdout, re.MULTILINE)
    summary = "OWNED_D3D12_RAYTRACING_ATOMIC64 lanes=64 words=512 stages=raygen,closesthit,miss,callable status=PASS"
    if (cases != ["PASS"] or stdout.splitlines().count(summary) != 1
            or stdout.splitlines().count("OWNED_D3D12_RAYTRACING_ATOMIC64_PASS") != 1):
        failures.append("atomic64 RT requires exactly one completed returning-atomic GPU oracle")
    lanes = re.findall(r"^OWNED_D3D12_RAYTRACING_ATOMIC64_LANE lane=(\d+) words=\("
                       + ",".join([r"0x([0-9a-f]{8})"] * 8) + r"\)$", stdout, re.MULTILINE)
    if ([int(row[0]) for row in lanes] != list(range(64))
            or stdout.count("OWNED_D3D12_RAYTRACING_ATOMIC64_LANE ") != 64):
        failures.append("atomic64 RT lane readback count/order/shape wrong")
    else:
        for lane, *values in lanes:
            index = int(lane)
            seed = ((0x24680000 + index) << 32) | (0xfedc0000 + index)
            tag = 0x73 if index & 1 else 0x35
            stage_old = seed + 0x200000021
            callable_old = stage_old + 0x100000007 + tag
            after = callable_old ^ 0x0102030405060708
            payload = (((index * 3 + 5) * 3) ^ stage_old ^ tag ^ callable_old ^ 0x91) & 0xffffffff
            expected = (after, seed, after, (payload << 32) | (2 if index & 1 else 1))
            expected_words = tuple(word for wide in expected for word in (wide & 0xffffffff, wide >> 32))
            if tuple(int(value, 16) for value in values) != expected_words:
                failures.append(f"atomic64 RT lane{index} returning RMW/CAS readback wrong")
    extended_rows = re.findall(r"^OWNED_D3D12_RAYTRACING_ATOMIC64_EXT_LANE mode=(\d+) lane=(\d+) words=\("
                              + ",".join([r"0x([0-9a-f]{8})"] * 20) + r"\)$", stdout, re.MULTILINE)
    if ([(int(row[0]), int(row[1])) for row in extended_rows] != [(mode, lane) for mode in range(2) for lane in range(24)]
            or stdout.count("OWNED_D3D12_RAYTRACING_ATOMIC64_EXT_LANE ") != 48):
        failures.append("atomic64 RT direct/indirect all-stage readback count/order/shape wrong")
    else:
        for mode in range(2):
            tickets = []
            for _, lane, *values in extended_rows[mode * 24:(mode + 1) * 24]:
                index = int(lane)
                kind = index % 3
                seed = ((0x24680000 + index) << 32) | (0xfedc0000 + index)
                value = seed + 0x200000021
                intersection_old = any_hit_old = 0
                if kind == 1:
                    intersection_old = value
                    value += 0x400000013
                if kind != 2:
                    any_hit_old = value
                    value += 0x800000017
                stage_old = value
                tag = (0x35, 0x57, 0x73)[kind]
                value += (0x300000009 if kind == 2 else 0x100000007) + tag
                callable_old = value
                final = value ^ 0x0102030405060708
                actual = [int(values[i], 16) | (int(values[i + 1], 16) << 32) for i in range(0, 20, 2)]
                tickets.append(actual[6])
                actual[6] = 0
                expected = [final, seed, any_hit_old, intersection_old, stage_old, callable_old, 0,
                            2 if kind == 2 else 1, 0x1000091 + (0x20000 if kind == 2 else 0x10001), tag]
                if actual != expected:
                    failures.append(f"atomic64 RT mode{mode} lane{index} all-stage old/final/once readback wrong")
            expected_tickets = sorted(((1 << 64) - 1 + index * 0x100000001) & ((1 << 64) - 1) for index in range(24))
            if sorted(tickets) != expected_tickets:
                failures.append(f"atomic64 RT mode{mode} shared-cell returning ticket multiset wrong")
    extended_shared = re.findall(r"^OWNED_D3D12_RAYTRACING_ATOMIC64_EXT_SHARED mode=(\d+) words=\("
                                + ",".join([r"0x([0-9a-f]{8})"] * 4) + r"\)$", stdout, re.MULTILINE)
    if extended_shared != [(str(mode), "00000017", "00000018", "0000005f", "00000000") for mode in range(2)]:
        failures.append("atomic64 RT shared-cell final value or stage-once counter wrong")
    for mode in range(2):
        extended_summary = f"OWNED_D3D12_RAYTRACING_ATOMIC64_EXT mode={mode} lanes=24 words=484 local=cbv stages=96 tickets=24 status=PASS"
        if stdout.splitlines().count(extended_summary) != 1:
            failures.append(f"atomic64 RT mode{mode} local-CBV/all-stage summary wrong")
    if re.search(r"OWNED_D3D12_RAYTRACING_(?:CASE_MISMATCH|FAIL)|kIOGPUCommandBufferCallbackError", stdout + stderr):
        failures.append("atomic64 RT fixture or GPU failure")
    return failures


def raytracing_failures(stdout: str, stderr: str) -> list[str]:
    """Re-derive the ray tracing readback contract from the fixture's printed evidence.

    The pass condition is the exact per-lane GPU readback the fixture observed, not the
    fixture's own verdict line: a Tier-0 capability report, an unsupported HRESULT, a
    dispatch that traced nothing, or a stale binary all leave the payload at the fill
    pattern and cannot produce these numbers.
    """
    failures: list[str] = []
    hitobject_cases = re.findall(r"^OWNED_D3D12_RAYTRACING_CASE hitobject status=(PASS|FAIL)$", stdout,
                                re.MULTILINE)
    if hitobject_cases != ["PASS"]:
        failures.append("raytracing requires exactly one completed HitObject GPU oracle")
    coherence_cases = re.findall(r"^OWNED_D3D12_RAYTRACING_CASE hitobject-reorder-coherence status=(PASS|FAIL)$",
                                stdout, re.MULTILINE)
    coherence_summary = re.findall(
        r"^OWNED_D3D12_RAYTRACING_HITOBJECT_COHERENCE lanes=(\d+) words=(\d+) scope=(\w+) wave_max=(\d+) status=(PASS|FAIL)$",
        stdout, re.MULTILINE)
    if (coherence_cases != ["PASS"] or len(coherence_summary) != 1
            or coherence_summary[0][:3] != ("64", "512", "reorder")
            or not 0 < int(coherence_summary[0][3]) < 64 or coherence_summary[0][4] != "PASS"):
        failures.append("raytracing requires one completed multi-wave REORDER_SCOPE GPU oracle")
    coherence_lanes = re.findall(
        r"^OWNED_D3D12_RAYTRACING_HITOBJECT_COHERENCE_LANE lane=(\d+) words=\("
        + ",".join([r"0x([0-9a-f]{8})"] * 8) + r"\)$", stdout, re.MULTILINE)
    if [int(lane[0]) for lane in coherence_lanes] != list(range(64)):
        failures.append("raytracing REORDER_SCOPE lane readback count/order wrong")
    else:
        for lane, *values in coherence_lanes:
            index = int(lane)
            seed = 0x43210000 + index * 0x105
            before = seed ^ 0x13579bdf
            second = (seed * 7 + 0x10203040) & 0xffffffff
            tag = 0x73 if index & 1 else 0x35
            payload = ((seed ^ 0x00ff00ff) + (second ^ 0x55aa55aa) + (before + tag) * 3) & 0xffffffff
            payload ^= 0x2468ace0
            post = (payload + 0x6a09e667) & 0xffffffff
            expected = (seed, post, before, before, payload, second, post ^ 0xbb67ae85, 2 if index & 1 else 1)
            if tuple(int(word, 16) for word in values) != expected:
                failures.append(f"raytracing REORDER_SCOPE lane{index} readback wrong")
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


def atomic64_phase_failures(stdout: str, stderr: str = "") -> list[str]:
    """Require actual old/final/canonical bytes and GPU-produced admission controls."""
    prefix = "OWNED_D3D12_ATOMIC64_PHASE_"
    expected: list[str] = [prefix + "BACKEND id=owned-graphics-d3d12"]
    for iteration, initial in enumerate((0x5555555500000000, 0x1234567800000003)):
        modified = 0x8765432100000000 | ((initial + 1) & 0xffffffff)
        values = (initial, initial + 0x100000001, modified, modified,
                  0x123456789abcdef0, 0x123456789abcdef0)
        expected.append(prefix + f"MAP iteration={iteration} words=" + ",".join(f"{v:016x}" for v in values))
    cases = ("indirect_nonzero", "indirect_count_zero", "indirect_dimension_zero", "indirect_predicate_zero",
             "crossqueue_atomic_to_ordinary", "crossqueue_ordinary_to_atomic")
    for case_index, case in enumerate(cases):
        work = case_index == 0 or case_index >= 4
        if case_index >= 4:
            before, after, submitted = (0, 10, 2) if case_index == 4 else (3, 7, 5)
            expected.append(prefix + f"ASYNC case={case} pending={before} completed={submitted - 1} "
                            f"submitted={submitted} event=258")
            expected.append(prefix + f"FENCE case={case} before={before} after={after} rewind=3 event=0 wait_first=1")
        cells = []
        for index in range(8):
            initial = ((0x12345678 + index) << 32) | (0xfffffff0 + index)
            delta = 65 if case_index == 0 and index >= 2 else 33
            final = initial + (0x100000000 + delta if work and index < 4 else 0)
            cells.append(final)
            if index < 4:
                old = initial if work else 0xcdcdcdcdcdcdcdcd
                returned = final if work else 0xcdcdcdcdcdcdcdcd
                root = 0xface0000 + int(case_index == 0 and index >= 2) if work else 0xcdcdcdcd
                expected.append(prefix + f"RETURN case={case} index={index} old=0x{old:016x} "
                                f"final=0x{returned:016x} root={root:08x}")
            expected.append(prefix + f"CELL case={case} index={index} copy=0x{final:016x} ordinary=0x{final:016x}")
        expected.append(prefix + f"COUNTS case={case} count={4 if work else 0} guard=a11c0ded")
        for index, offset in enumerate((4, 8, 12, 16, 4, 8, 4)):
            word = (cells[offset // 8] >> ((offset % 8) * 8)) & 0xffffffff
            expected.append(prefix + f"RAW case={case} index={index} word={word:08x}")
        if case_index < 4:
            for command in range(3):
                words = (command * 2, 65 if command else 33, 0xface0000 + command, 0,
                         0 if case_index == 2 else (1 if command else 2), 2 if command == 1 else 1, 1)
                for word, value in enumerate(words):
                    expected.append(prefix + f"ARGUMENT case={case} index={command * 7 + word} word={value:08x}")
            expected.append(prefix + f"CONTROL case={case} count={0 if case_index == 1 else 2} "
                            f"predicate={0 if case_index == 3 else 1}")
    expected.extend((prefix + "RESULT failures=0", prefix + "PASS indirect=4 crossqueue=2"))
    actual = [line for line in stdout.splitlines() if line.startswith(prefix)]
    failures = [] if actual == expected else ["atomic64 phase numeric readback/admission/fence evidence differs"]
    if any(marker in stdout + stderr for marker in ("_MISMATCH", "_CASE_FAIL", "_FAIL", "SM6_FATAL")):
        failures.append("atomic64 phase fixture failure")
    return failures


def query_ordering_failures(stdout: str) -> list[str]:
    failures = []
    if stdout.count("OWNED_D3D12_QUERY_BOP_PASS rounds=8 separate_execute=1 same_slot_reuse=1 "
                    "inter_submission_cpu_waits=0") != 1:
        failures.append("missing cross-submission timestamp scenario")
    values = re.findall(r"^OWNED_D3D12_QUERY_BOP_VALUES round=(\d+) end=(\d+) successor=(\d+) reused=(\d+)$",
                        stdout, re.MULTILINE)
    if [int(row[0]) for row in values] != list(range(8)):
        failures.append("timestamp rounds missing, duplicated, or reordered")
    previous = 0
    for round_index, end, successor, reused in values:
        end, successor, reused = int(end), int(successor), int(reused)
        if not (0 < end <= successor <= reused < 2**64 - 1) or end < previous:
            failures.append(f"timestamp ordering violated in round {round_index}")
        previous = reused
    if stdout.count("OWNED_D3D12_QUERY_COPY_CAP supported=1") != 1:
        failures.append("COPY timestamp capability missing")
    if stdout.count("OWNED_D3D12_QUERY_COPY_PASS rounds=8 separate_execute=1 same_slot_reuse=1 "
                    "duplicate_resolve=1 guards=1 payload_bytes=524288 inter_submission_cpu_waits=0 "
                    "final_fence=1 release_before_completion=1") != 1:
        failures.append("COPY timestamp workload/resolve scenario missing")
    calibration = re.findall(r"^OWNED_D3D12_QUERY_COPY_CALIBRATION frequency=(\d+) gpu_before=(\d+) "
                             r"gpu_after=(\d+) cpu_before=(\d+) cpu_after=(\d+) qpc_bracket=1$",
                             stdout, re.MULTILINE)
    if len(calibration) != 1:
        failures.append("COPY calibration sample missing or duplicated")
        return failures
    frequency, before, after, cpu_before, cpu_after = map(int, calibration[0])
    if not (frequency > 0 and 0 < before <= after < 2**64 - 1 and 0 < cpu_before <= cpu_after):
        failures.append("COPY frequency or calibration interval invalid")
    copy_values = re.findall(r"^OWNED_D3D12_QUERY_COPY_VALUES round=(\d+) pre=(\d+) post=(\d+) "
                             r"successor=(\d+) duplicate=(\d+) reused=(\d+) guards=1$",
                             stdout, re.MULTILINE)
    if [int(row[0]) for row in copy_values] != list(range(8)):
        failures.append("COPY timestamp rounds missing, duplicated, or reordered")
    previous = before
    for round_index, pre, post, successor, duplicate, reused in copy_values:
        pre, post, successor, duplicate, reused = map(int, (pre, post, successor, duplicate, reused))
        if not (previous <= pre <= post <= successor <= reused <= after) or duplicate != post:
            failures.append(f"COPY timestamp ordering, domain or duplicate resolve violated in round {round_index}")
        previous = reused
    return failures


def dxil_resource_review_failures(stdout: str) -> list[str]:
    failures = []
    checks = (("bounds", 26), ("gradients", 6), ("array-atomics", 24), ("bounds64", 17))
    cases = re.findall(r"^OWNED_D3D12_DXIL_RESOURCE_REVIEW_CASE_PASS case=([\w-]+) checks=(\d+)$",
                       stdout, re.MULTILINE)
    if cases != [(name, str(count)) for name, count in checks]:
        failures.append("shader review cases or checked boundary counts incomplete")
    words = re.findall(r"^OWNED_D3D12_DXIL_RESOURCE_REVIEW_WORD case=([\w-]+) index=(\d+) "
                       r"actual=0x([0-9a-f]{16}) expected=0x([0-9a-f]{16})$", stdout, re.MULTILINE)
    if [(name, int(index)) for name, index, _, _ in words] != [
            (name, index) for name, count in checks for index in range(count)]:
        failures.append("shader review readback words missing, duplicated, or reordered")
    if any(actual != expected for _, _, actual, expected in words):
        failures.append("shader review readback differs from CPU oracle")
    return failures


def relaxed_casting_failures(suite: str, stdout: str, stderr: str = "") -> list[str]:
    failures = []
    caps = re.findall(r"^RELAXED_CASTING_OPTIONS12 relaxed=([01])$", stdout, re.MULTILINE)
    if caps != ["1"]:
        failures.append("relaxed casting capability query missing or not promoted")
    cases = [tuple(map(int, match)) for match in re.findall(
        r"^RELAXED_CASTING_CASE base=(\d+) target=(\d+) bc=([01]) placed=([01]) legacy=([01]) "
        r"writable=([01]) sparse=([01]) shared=([01]) mip=1 slice=1 words=exact replay=([23])$",
        stdout, re.MULTILINE)]
    volumes = [(int(base), int(unit), int(placed), int(shared), int(layout), int(writable), alias)
               for base, unit, placed, shared, layout, writable, alias in re.findall(
        r"^RELAXED_CASTING_VOLUME_CASE base=(\d+) unit=(\d+) placed=([01]) shared=([01]) layout=(\d+) writable=([01]) "
        r"alias=(none|same-dispatch-exact) mip=1 depth=32 "
        r"words=exact nativeBC=exact nativecopy=exact replay=2$", stdout, re.MULTILINE)]
    if suite == "relaxed-casting":
        formats = ((28, 42, 0, 0, 0), (11, 18, 0, 0, 0), (3, 2, 0, 0, 0),
                   (71, 17, 1, 0, 0), (98, 3, 1, 0, 0), (41, 41, 0, 1, 0),
                   (39, 42, 0, 1, 0), (13, 13, 0, 1, 0), (28, 42, 0, 0, 1),
                   (79, 81, 0, 1, 0), (71, 17, 1, 0, 1))
        expected = [(base, target, bc, placed, legacy, writable, 0, 0, 2)
                    for placed in (0, 1) for base, target, bc, legacy, writable in formats]
        expected += [(71, 17, 1, 0, 0, 0, 1, 0, 3), (71, 17, 1, 0, 0, 1, 1, 0, 2)]
        expected_volumes = []
        if stdout.count("RELAXED_CASTING_NEGATIVES nonBC_to_BC=reject planar=reject unequal_unit=reject\n") != 1:
            failures.append("relaxed casting creation rejection controls missing")
        marker = "RELAXED_CASTING_PASS cases=24 GPU=exact"
    elif suite == "relaxed-casting-shared":
        expected = [(28, 42, 0, 0, 0, 0, 0, 1, 2), (28, 42, 0, 0, 0, 1, 0, 1, 2),
                    (71, 17, 1, 0, 0, 1, 0, 1, 2)]
        expected_volumes = [(base, unit, 0, 1, 0, 1, "same-dispatch-exact") for base, unit in ((71, 8), (98, 16))]
        marker = "RELAXED_CASTING_SHARED_PASS cases=5 GPU=exact"
    else:
        expected = []
        standard = suite == "relaxed-casting-volume-standard"
        expected_volumes = [(base, unit, placed, 0, 3 if standard else 0, writable,
                             "same-dispatch-exact" if writable else "none")
                            for base, unit in ((71, 8), (98, 16)) for writable in (0, 1) for placed in (0, 1)]
        marker = f"RELAXED_CASTING_VOLUME{'_STANDARD' if standard else ''}_PASS cases=8 GPU=exact"
    if cases != expected or volumes != expected_volumes:
        failures.append("relaxed casting exact numerical/alias/sharing case matrix incomplete")
    if len(re.findall(r"^RELAXED_CASTING_CASE.*$", stdout, re.MULTILINE)) != len(cases) or \
            len(re.findall(r"^RELAXED_CASTING_VOLUME_CASE.*$", stdout, re.MULTILINE)) != len(volumes):
        failures.append("relaxed casting malformed or extra numerical case")
    markers = re.findall(r"^RELAXED_CASTING(?:_SHARED|_VOLUME(?:_STANDARD)?)?_PASS.*$", stdout, re.MULTILINE)
    if markers != [marker] or "RELAXED_CASTING_FAIL" in stdout + stderr:
        failures.append("relaxed casting terminal proof missing or fixture failed")
    return failures


def pipeline_cache_failures(stdout: str, stderr: str, logs: dict[str, str]) -> list[str]:
    """Count identity-keyed automatic producers, not GPU helper PSO compiles."""
    prefix = "OWNED_D3D12_PIPELINE_CACHE"
    failures = []
    cases = re.findall(rf"^{prefix}_CASE_PASS (\w+)$", stdout, re.MULTILINE)
    if cases != ["ordinary_key_memory", "disk_restore_invalid_rebuild", "concurrent_single_producer",
                 "failure_completion_retry", "driver_control_app_independence"]:
        failures.append("automatic pipeline cache case matrix missing, duplicated or reordered")
    children = re.findall(rf"^{prefix}_CHILD_PASS mode=(\w+)$", stdout, re.MULTILINE)
    if children != ["disk", "corrupt", "truncated", "identity", "native"]:
        failures.append("same-executable disk child matrix incomplete")
    if stdout.splitlines().count(prefix + "_NATIVE_FAULT field=metallib_version_major before=2 after=65535") != 1:
        failures.append("native-consumed metallib version mutation evidence missing")
    add, xor = 0x31415926 + 0x135, 0x31415926 ^ 0x135
    expected_gpu = {label: add for label in ("ordinary_a", "ordinary_root", "memory_a", "memory_root",
                    "child_disk", "child_corrupt", "child_truncated", "child_identity", "child_native",
                    "failure_valid_control", "disabled_first", "disabled_second", "enabled", "clear_rebuild",
                    "unaligned_input_lifetime", "returned_blob_isolation")}
    expected_gpu.update({label: xor for label in ("ordinary_b", "memory_b", "changed_shader_control")})
    expected_gpu.update({f"concurrent_{i}": add for i in range(8)})
    gpu = re.findall(rf"^{prefix}_GPU case=(\w+) actual=([0-9a-f]{{8}}) expected=([0-9a-f]{{8}})$",
                     stdout, re.MULTILINE)
    if (len(gpu) != len(expected_gpu) or {label for label, _, _ in gpu} != set(expected_gpu)
            or any(int(actual, 16) != expected_gpu.get(label) or int(wanted, 16) != expected_gpu.get(label)
                   for label, actual, wanted in gpu)):
        failures.append("automatic/explicit pipeline cache exact GPU output matrix incomplete")
    rejected = re.findall(rf"^{prefix}_REJECT case=(\w+) hr=([0-9a-f]{{8}})$", stdout, re.MULTILINE)
    if rejected != [("pre_p3_version", "887e0002"), *[(name, "80070057") for name in
                    ("null_nonempty", "truncated", "corrupt_payload", "foreign_blob", "root_mismatch", "shader_mismatch")]]:
        failures.append("explicit CachedPSO rejection contract changed or incomplete")
    keys = re.findall(rf"^{prefix}_KEY case=(\w+) key=([0-9a-f]{{40}})$", stdout, re.MULTILINE)
    named_keys = {name: key for name, key in keys if name != "child"}
    if (len(keys) != 11 or set(named_keys) != {"a", "b", "root", "concurrent", "control", "control_rebuilt"}
            or len({named_keys.get(name) for name in ("a", "b", "root", "concurrent")}) != 4
            or named_keys.get("control") != named_keys.get("control_rebuilt")
            or any(key != named_keys.get("a") for name, key in keys if name == "child")):
        failures.append("automatic identity key/blob disk mapping incomplete or aliased")
    phases: dict[str, list[dict]] = {}
    for name, text in logs.items():
        phase = name.split("/", 1)[0] if "/" in name else "parent"
        events = phases.setdefault(phase, [])
        for line in text.splitlines():
            if "OWNED_GRAPHICS_TRACE " not in line:
                continue
            try:
                event = json.loads(line.split("OWNED_GRAPHICS_TRACE ", 1)[1])
            except (json.JSONDecodeError, ValueError):
                continue  # The common trace reader reports malformed JSON.
            if isinstance(event, dict):
                events.append(event)
    parent = phases.get("parent", [])
    for name in ("a", "b", "root", "concurrent"):
        events = [event for event in parent if event.get("key") == named_keys.get(name)]
        begins = sum(event.get("event") == "pipeline_cache_auto_begin" for event in events)
        publishes = [event for event in events if event.get("event") == "pipeline_cache_auto_publish"]
        hits = sum(event.get("event") == "pipeline_cache_auto_hit" and event.get("source") == "memory"
                   for event in events)
        if (begins != 1 or len(publishes) != 1 or publishes[0].get("hr") != 0
                or publishes[0].get("bytes", 0) <= 0 or (name != "concurrent" and hits < 1)
                or any(event.get("event") == "pipeline_cache_auto_failure" for event in events)):
            failures.append(f"automatic {name}: producers={begins}, publishes={len(publishes)}, memory_hits={hits}")
    for phase in ("disk", "corrupt", "truncated", "identity", "native"):
        events = [event for event in phases.get(f"pipeline-cache-{phase}", [])
                  if event.get("key") == named_keys.get("a")]
        begins = sum(event.get("event") == "pipeline_cache_auto_begin" for event in events)
        disk_hits = sum(event.get("event") == "pipeline_cache_auto_hit" and event.get("source") == "disk"
                        for event in events)
        publishes = sum(event.get("event") == "pipeline_cache_auto_publish" and event.get("hr") == 0
                        and event.get("bytes", 0) > 0 for event in events)
        retries = sum(event.get("event") == "pipeline_cache_auto_retry" for event in events)
        expected = (0, 1, 0, 0) if phase == "disk" else (0, 1, 1, 1) if phase == "native" else (1, 0, 1, 0)
        if (begins, disk_hits, publishes, retries) != expected:
            failures.append(f"automatic child {phase}: producers={begins}, disk_hits={disk_hits}, publishes={publishes}, retries={retries}")
    failed_events = phases.get("pipeline-cache-failure", [])
    failed = [event for event in failed_events if event.get("event") == "pipeline_cache_auto_failure"]
    failed_keys = {event.get("key") for event in failed}
    if len(failed_keys) != 1 or len(failed) < 2:
        failures.append("failed producer completion and later retry evidence missing")
    else:
        key = next(iter(failed_keys))
        begins = sum(event.get("event") == "pipeline_cache_auto_begin" and event.get("key") == key
                     for event in failed_events)
        if (begins != len(failed) or any(event.get("hr") == 0 or event.get("bytes") != 0 for event in failed)
                or any(event.get("key") == key and event.get("event") in
                       ("pipeline_cache_auto_hit", "pipeline_cache_auto_publish") for event in failed_events)):
            failures.append("failed automatic key was cached or left an incomplete producer")
    control = phases.get("pipeline-cache-control", [])
    events = [event for event in control if event.get("key") == named_keys.get("control")]
    controls = [event for event in control if event.get("event") == "pipeline_cache_auto_control"]
    # Public flags are DISABLE=1, ENABLE=2, CLEAR=4. Require successful coverage,
    # not an incidental call order; key-scoped effects below prove actual behavior.
    control_flags = [event.get("flags") for event in controls]
    controls_ok = (bool(controls) and all(event.get("hr") == 0 for event in controls)
                   and all(type(flags) is int and 0 < flags <= 7 and flags & 3 != 3 for flags in control_flags)
                   and all(any(flags & required for flags in control_flags) for required in (1, 2, 4)))
    if (sum(event.get("event") == "pipeline_cache_auto_begin" for event in events) != 2
            or sum(event.get("event") == "pipeline_cache_auto_publish" and event.get("hr") == 0
                   for event in events) != 2
            or sum(event.get("event") == "pipeline_cache_auto_hit" and event.get("source") == "memory"
                   for event in events) != 1
            or sum(event.get("event") == "pipeline_cache_auto_bypass" and event.get("reason") == "disabled"
                   for event in events) != 2
            or not controls_ok):
        failures.append("driver-managed enable/disable/clear trace evidence incomplete")
    if stdout.splitlines().count(prefix + "_PASS") != 1 or prefix + "_FAIL" in stdout + stderr:
        failures.append("pipeline cache terminal proof missing or fixture failed")
    return failures


def options13_failures(stdout: str, stderr: str = "") -> list[str]:
    """Require the full public contract matrix; a diagnostic subset is not a suite pass."""
    prefix = "OWNED_D3D12_OPTIONS13"
    failures = []
    expected_cases = ["alpha_blend", "viewport_z", "viewport_y", "vertex_alignment",
                      "copy_pitch", "cross_dimension", "triangle_fan", "manual_write_tracking", "capabilities"]
    for phase in ("BEGIN", "PASS"):
        cases = re.findall(rf"^{prefix}_CASE_{phase} (\w+)$", stdout, re.MULTILINE)
        if cases != expected_cases:
            failures.append(f"OPTIONS13 {phase.lower()} cases missing, duplicated or reordered")
    formats = (("r8", 1), ("r16", 2), ("rgba8", 4), ("rgba16", 8), ("rgba32", 16),
               ("rgb32", 12), ("bc1", 8), ("bc3", 16), ("bc7", 16))
    copies = re.findall(rf"^{prefix}_COPY format=(\w+) dims=(\d+) queue=(\d+) bytes=(\d+) guards=1$",
                        stdout, re.MULTILINE)
    expected_copies = [(name, str(dims), str(queue), str(size))
                       for queue in (0, 2, 3) for name, size in formats
                       for dims in ((2,) if name.startswith("bc") else (1, 2, 3))]
    if copies != expected_copies:
        failures.append("OPTIONS13 texel/block copy matrix incomplete or row guards unchecked")
    if stdout.splitlines().count(f"{prefix}_COPY_BOUNDS rejected=6 replay=63") != 1:
        failures.append("OPTIONS13 copy replay or negative bounds proof missing")
    cross = re.findall(rf"^{prefix}_CROSS from=(\d+) to=(\d+) offsets=1 guards=1$", stdout, re.MULTILINE)
    if cross != [(str(source), str(destination)) for source in (1, 2, 3)
                 for destination in (1, 2, 3) if source != destination]:
        failures.append("OPTIONS13 cross-dimensional copies incomplete or untouched regions unchecked")
    if stdout.splitlines().count(f"{prefix}_CROSS_BOUNDS rejected=18 replay=6") != 1:
        failures.append("OPTIONS13 cross-dimensional replay or negative bounds proof missing")
    fan = re.findall(rf"^{prefix}_FAN row=(\w+) pixels=64 colored=(\d+) ia=(\d+) gs=(\d+) samples=(\d+) replay=1$",
                     stdout, re.MULTILINE)
    fan_names = ["direct_empty", "direct_one", "direct_two", "direct_triangle", "direct_start_instances",
                 "direct_zero_instances", "indexed16_offset_base", "indexed32_negative_base",
                 "indexed16_restart", "indexed32_restart_gs", "indexed16_cut_disabled", "indirect_draw",
                 "indirect_indexed_count", "bundle_direct_twice", "bundle_indexed16", "gs_input_order",
                 "backface_culled", "degenerate_count"]
    if [row[0] for row in fan] != fan_names or [int(row[2]) for row in fan] != [0, 0, 0, 1, 8, 0, 4, 4, 4, 4, 1, 8, 8, 8, 4, 4, 4, 4] or any(
            int(colored) > 64 or int(samples) > 64 * int(ia) or
            int(gs) != (int(ia) if name in ("indexed32_restart_gs", "gs_input_order") else 0)
            for name, colored, ia, gs, samples in fan):
        failures.append("OPTIONS13 fan rows, CPU pixels, primitive statistics or occlusion proof incomplete")
    required = [
        f"{prefix}_FAN_ORACLE rows=18 pixels=1152 primitives=66 cpu=1 winding=1 provoking=1 restart=1 indirect=1 bundles=1 gs=1 queries=1 occlusion=1 replay=1",
        f"{prefix}_MANUAL_TRACKING public=0 committed_rejected=1 heap_rejected=1 tools_qi=absent",
        *[f"{prefix}_IMMEDIATE queue={queue} snapshots=3 bytes=192 modes=3 replay=1 changed_cpu_input=1 guards=1"
          for queue in (0, 2, 3)],
        f"{prefix}_MANUAL_ORACLE public=0 rejected=2 tools_qi=absent immediate_queues=3 snapshots=9 replay=3 cpu=1",
    ]
    if any(stdout.splitlines().count(line) != 1 for line in required):
        failures.append("OPTIONS13 fan oracle or public-false/manual-immediate contract missing")
    for tag, count in (("COPY", 63), ("CROSS", 6), ("FAN", 18), ("IMMEDIATE", 3),
                       ("COPY_BOUNDS", 1), ("CROSS_BOUNDS", 1), ("FAN_ORACLE", 1),
                       ("MANUAL_TRACKING", 1), ("MANUAL_ORACLE", 1)):
        if len(re.findall(rf"^{prefix}_{tag}(?: |$).*$", stdout, re.MULTILINE)) != count:
            failures.append(f"OPTIONS13 malformed or extra {tag} evidence")
    if re.findall(rf"^{prefix}_PASS cases=(\d+)$", stdout, re.MULTILINE) != ["9"]:
        failures.append("OPTIONS13 complete summary missing or duplicated")
    if f"{prefix}_FAIL" in stdout + stderr:
        failures.append("OPTIONS13 fixture reported a contract failure")
    return failures


def corewindow_composition_evidence(stdout: str, stderr: str = "") -> dict:
    """Genuine CoreWindow remains separate from the positive Composition suite."""
    prefix = SUITE_PREFIXES["corewindow-composition"]
    lines = stdout.splitlines()
    failures = []
    rows = re.findall(r"^" + prefix + r"_CONTRACT step=([\w-]+) hr=0x([0-9a-f]{8})$",
                      stdout, re.MULTILINE)
    steps = ["null-corewindow", "non-corewindow-object", "hwnd-no-corewindow",
             "hwnd-no-matrix", "hwnd-no-matrix-get", "hdr10plus", "stereo-swapchain",
             "actual-corewindow-runtime"]
    contracts = dict(rows)
    if ([step for step, _ in rows] != steps
            or sum(line.startswith(prefix + "_CONTRACT") for line in lines) != len(steps)):
        failures.append("CoreWindow/Composition reachable contract inventory missing, duplicated or malformed")
    for step in steps[:5]:
        if contracts.get(step) != "887a0001":
            failures.append(f"CoreWindow/Composition {step} must reject with DXGI_ERROR_INVALID_CALL")
    if contracts.get("hdr10plus") != "887a0004":
        failures.append("CoreWindow unsupported HDR contract mismatch")
    stereo = contracts.get("stereo-swapchain")
    if stereo is None or int(stereo, 16) < 0x80000000:
        failures.append("CoreWindow/Composition stereo creation must fail")
    required = [prefix + "_BACKEND id=owned-graphics-d3d12",
                prefix + "_CAPS hardware=0 overlay=0 overlays=0 stereo=0",
                prefix + "_REACHABLE_OK corewindow=separate-prerequisite"]
    if any(lines.count(line) != 1 for line in required):
        failures.append("CoreWindow/Composition exact backend, capability or reachable terminal missing/duplicated")
    actual_rows = re.findall(r"^" + prefix + r"_COREWINDOW actual=([01]) gpu_present=([01])$",
                             stdout, re.MULTILINE)
    actual = int(actual_rows[0][0]) if len(actual_rows) == 1 else None
    if len(actual_rows) != 1 or actual_rows[0][0] != actual_rows[0][1]:
        failures.append("CoreWindow/Composition actual object/GPU-present evidence missing or inconsistent")
    missing = prefix + "_PREREQUISITE corewindow=coreapplication-ui-thread-window-required"
    prerequisite_rows = [line for line in lines if line.startswith(prefix + "_PREREQUISITE")]
    if prerequisite_rows != ([missing] if actual == 0 else []):
        failures.append("CoreWindow/Composition concrete CoreWindow prerequisite evidence inconsistent")
    runtime = contracts.get("actual-corewindow-runtime")
    if actual == 1 and (runtime is None or int(runtime, 16) >= 0x80000000):
        failures.append("CoreWindow/Composition cannot present a failed CoreWindow runtime result")
    expected_lines = [prefix + f"_CONTRACT step={step} hr=0x{hr}" for step, hr in rows]
    expected_lines += required + prerequisite_rows
    if len(actual_rows) == 1:
        expected_lines.append(prefix + f"_COREWINDOW actual={actual_rows[0][0]} gpu_present={actual_rows[0][1]}")
    if (Counter(line for line in lines if line.startswith(prefix)) != Counter(expected_lines)
            or prefix + "_FAIL" in stdout + stderr):
        failures.append("CoreWindow/Composition extra, duplicate or failed producer evidence")
    return {"actual_corewindow": actual, "corewindow_runtime_hresult": runtime,
            "composition": "separate-suite", "matrix_pixel_oracle": "separate-suite",
            "feature_complete": False,
            "prerequisites": {
                "corewindow": ("coreapplication-ui-thread-window-required" if actual == 0 else
                               "actual-runtime-object-presented" if actual == 1 else "unverified"),
                "composition": "run --suite composition independently"},
            "failures": failures}


def composition_evidence(stdout: str, stderr: str = "", *, visual: bool = False) -> dict:
    """Source readback is not an oracle for CoreAnimation matrix or alpha blending."""
    prefix = SUITE_PREFIXES["composition"]
    lines = stdout.splitlines()
    failures = []
    cases = ["godot_alpha_present", "matrix_next_present_validation", "no_hwnd_restrictions",
             "source_resize", "commit_detach_ownership", "queued_present_content_window_lifetime",
             "ordinary_hwnd_regression"]
    expected = [prefix + "_BACKEND id=owned-graphics-d3d12"]
    expected += [prefix + f"_CASE_PASS case={name}" for name in cases]
    gpu = [(name, width, height) for name, width, height in (
        ("premul", 80, 64), ("straight", 80, 64), ("matrix_premul", 80, 64),
        ("matrix_straight", 80, 64), ("source_premul", 80, 64), ("source_straight", 80, 64),
        ("resize_premul", 96, 80), ("resize_straight", 96, 80),
        ("queued_a_premul", 96, 80), ("queued_b_premul", 96, 80),
        ("queued_a_straight", 96, 80), ("queued_b_straight", 96, 80),
        ("destroyed_window", 80, 64), ("ordinary_hwnd", 80, 64))]
    expected += [prefix + f"_GPU case={name} size={width}x{height} samples=5 readback=exact"
                 for name, width, height in gpu]
    observed_cases = re.findall(r"^" + prefix + r"_CASE_PASS case=([a-z_]+)$", stdout, re.MULTILINE)
    if observed_cases != cases:
        failures.append("Composition public positive case inventory missing, duplicated or reordered")
    observed_gpu = re.findall(r"^" + prefix + r"_GPU case=([a-z_]+) size=(\d+)x(\d+) samples=5 readback=exact$", stdout, re.MULTILINE)
    if observed_gpu != [(name, str(width), str(height)) for name, width, height in gpu]:
        failures.append("Composition source GPU readbacks missing, duplicated or reordered")
    terminal = "external-capture-required" if visual else "not-observed"
    expected.append(prefix + f"_PASS cases=7 gpu=14 visual={terminal} corewindow=separate-prerequisite")
    queue_order = [prefix + "_QUEUE_ORDER phase=before_release completed_b=0 completed_a=0",
                   prefix + "_QUEUE_ORDER phase=after_release completed_b=1 completed_a=1"]
    if [line for line in lines if line.startswith(prefix + "_QUEUE_ORDER")] != queue_order:
        failures.append("Composition blocked-predecessor queue completion evidence missing or inconsistent")
    expected += queue_order
    phases = ["identity", "matrix-pending", "matrix-presented", "source-cropped", "resized",
              "content-pending", "content-detached", "root-pending", "root-detached", "reattached",
              "queued-before-release", "queued-captured-identity", "content-lifetime"]
    ready = re.findall(r"^" + prefix + r"_VISUAL_READY phase=([a-z-]+) left=(?:0x)?([0-9a-fA-F]+) right=(?:0x)?([0-9a-fA-F]+) control=(.+)$", stdout, re.MULTILINE)
    advanced = re.findall(r"^" + prefix + r"_VISUAL_ADVANCED phase=([a-z-]+)$", stdout, re.MULTILINE)
    if visual:
        if [phase for phase, _, _, _ in ready] != phases or advanced != phases:
            failures.append("Composition external-capture holds incomplete or reordered")
        hold_order = re.findall(r"^" + prefix + r"_VISUAL_(READY|ADVANCED) phase=([a-z-]+)(?: |$)", stdout, re.MULTILINE)
        if (hold_order != [(kind, phase) for phase in phases for kind in ("READY", "ADVANCED")]
                or sum(line.startswith(prefix + "_VISUAL_READY") for line in lines) != len(ready)):
            failures.append("Composition malformed or out-of-order visual control records")
        if any(int(left, 16) == 0 or int(right, 16) == 0 or left == right for _, left, right, _ in ready):
            failures.append("Composition visual holds require two distinct real windows")
        expected += [line for line in lines if line.startswith(prefix + "_VISUAL_READY ")]
        expected += [prefix + f"_VISUAL_ADVANCED phase={phase}" for phase in phases]
    if (Counter(line for line in lines if line.startswith(prefix)) != Counter(expected)
            or prefix + "_FAIL" in stdout + stderr):
        failures.append("Composition extra, malformed, duplicate or failed producer evidence")
    return {"public_cases": observed_cases, "source_gpu_readbacks": len(observed_gpu),
            "visual_holds": [phase for phase, _, _, _ in ready],
            "onscreen_alpha_matrix": "external-capture-required" if visual else "not-observed",
            "feature_complete": False, "corewindow": "separate-prerequisite", "failures": failures}


def composition_module_evidence(stderr: str, runtime: Path, manifest: dict, arch: dict) -> dict:
    """Require the selected same-ABI owned Wine frontend and its actual builtin load."""
    failures = []
    directory = "lib/wine/i386-windows" if arch["guest_arch"] == "x86" else arch["windows_directory"]
    relative = str(Path(directory) / "dcomp.dll")
    frontend = manifest.get("wine_display_frontend", {})
    record = frontend.get("modules", {}).get(relative)
    image = runtime / relative
    rows = re.findall(r'Loaded L"([^"\n]*dcomp\.dll)"[^\n]*(native|builtin)\b', stderr, re.IGNORECASE)
    if (manifest.get("optional_modules", {}).get("dcomp") is not True
            or frontend.get("source_pin") != "913e31f201d344223bdf3d13a50a41af35893d12"
            or not isinstance(record, dict)):
        failures.append("Composition selected owned dcomp frontend manifest missing")
    else:
        try:
            abi = "x86" if arch["guest_arch"] == "x86" else arch["windows_abi"]
            if image.is_symlink() or not image.is_file() or digest(image) != record.get("sha256") or inspect_pe(image)["windows_abi"] != abi:
                failures.append("Composition frontend path/bytes/guest ABI mismatch")
        except (OSError, ValueError) as error:
            failures.append(str(error))
    allowed = {r"c:\windows\system32\dcomp.dll", r"c:\windows\syswow64\dcomp.dll",
               ("Z:" + str(image).replace("/", "\\")).casefold()}
    if not rows or any(mode.lower() != "builtin" or path.replace("\\\\", "\\").casefold() not in allowed for path, mode in rows):
        failures.append("Composition actual owned builtin dcomp load missing or foreign")
    return {"module": relative, "loads": [{"path": path, "mode": mode} for path, mode in rows],
            "failures": failures}

def game_suite_failures(suite: str, stdout: str, stderr: str = "") -> list[str]:
    """Validate public fixture protocols; synthetic parser tests are not GPU proof."""
    failures = []
    prefix = SUITE_PREFIXES[suite]
    lines = stdout.splitlines()
    if "_FAIL" in stdout + stderr or "_EXPECT" in stdout + stderr:
        failures.append("game fixture reported a failure")
    if suite in ("ngx-fg", "ngx-features"):
        rows = re.findall(r"^OWNED_D3D12_NGX_NATIVE frame=(\d+) reset=([01]) replay=([01]) output_written=1 different_current=([01]) resources_released=1 consumer=gpu_readback frame_id=(\d+) not_rendering=([01])$", stdout, re.MULTILINE)
        expected = [(frame, replay) for frame in range(9) for replay in range(2 if frame in (0, 7, 8) else 1)]
        if ([(int(f), int(r)) for f, _, r, *_ in rows] != expected
                or sum(line.startswith("OWNED_D3D12_NGX_NATIVE") for line in lines) != len(expected)):
            failures.append("NGX requires every ordered native output and retained-resource replay")
        for frame, reset, replay, different, frame_id, not_rendering in rows:
            frame = int(frame)
            if (int(reset) != int(frame in (0, 5)) or int(frame_id) != (7 if frame == 8 else frame)
                    or int(not_rendering) != int(frame == 6)
                    or (replay == "0" and frame in (3, 4) and different != "1")):
                failures.append("NGX native history/reset or genuine output proof mismatch")
                break
        rejected = [line for line in lines if line.startswith("OWNED_D3D12_NGX_REJECT ")]
        expected_rejections = [f"OWNED_D3D12_NGX_REJECT case={kind} {kind}={value} result=unsupported_parameter"
                               for kind in ("count", "index") for value in (2, 3, 4)]
        expected_rejections.append("OWNED_D3D12_NGX_REJECT case=disable_buffer result=unsupported_parameter output_untouched=1 before_valid_dispatch=1 zero_count_index=invalid_parameter")
        if rejected != expected_rejections:
            failures.append("NGX native capability rejection or untouched-output evidence incomplete")
        if [line for line in lines if line.startswith("OWNED_D3D12_NGX_SCENE")] != [
                "OWNED_D3D12_NGX_SCENE frames=14 reset=1 output_written=1 actual_count=unavailable consumer=gpu_readback"]:
            failures.append("NGX native explicit scene-reset output evidence incomplete")
    if suite == "sr-alpha":
        rows = re.findall(r"^OWNED_D3D12_SR_ALPHA_CASE_PASS frame=(\d+) replay=(\d+) scalar_oracle_max_half_ulp=(\d+) subrect=1 dynamic=([01]) retired=([01])$", stdout, re.MULTILINE)
        expected = [(str(frame), str(replay), str(int(frame == 3)), str(int(frame == 5)))
                    for frame in range(6) for replay in range(2 if frame == 5 else 1)]
        if [(f, r, d, t) for f, r, _, d, t in rows] != expected or any(int(u) > 4 for _, _, u, _, _ in rows):
            failures.append("SR alpha scalar oracle, dynamic subrect or retirement replay incomplete")
        terminal = re.findall(r"^OWNED_D3D12_SR_ALPHA_PASS frames=6 replay_after_release=1 independent_scalar_oracle=1 history_delta=(\d+)$", stdout, re.MULTILINE)
        if len(terminal) != 1 or int(terminal[0]) == 0:
            failures.append("SR alpha history stimulus proof missing")
        case_count = 7
    elif suite == "fsr-debug":
        rows = re.findall(r"^OWNED_D3D12_FSR_DEBUG_CASE_PASS extent=(\d+) replay=(\d+) panel_max_byte_error=(\d+) sentinel=1 retired=([01])$", stdout, re.MULTILINE)
        if [(e, r, t) for e, r, _, t in rows] != [(str(e), str(r), str(int(e == 96))) for e in (128, 96) for r in (0, 1)] or any(int(error) > 2 for _, _, error, _ in rows):
            failures.append("FSR debug real-input numerical oracle or retirement replay incomplete")
        if lines.count("OWNED_D3D12_FSR_DEBUG_PASS public_dispatch=1 depth_gradient=1 signed_motion=1 color=1 dynamic=1 retirement_replay=1") != 1:
            failures.append("FSR debug summary missing")
        case_count = 4
    elif suite == "ngx-alpha":
        rows = re.findall(r"^OWNED_D3D12_NGX_ALPHA_PIXELS replay=([01]) left=(\d+),(\d+),(\d+),(\d+) right=(\d+),(\d+),(\d+),(\d+) rgb_max_error=(\d+) release_before_execute=1$", stdout, re.MULTILINE)
        values = [tuple(map(int, row)) for row in rows]
        if [row[0] for row in values] != [0, 1] or len([line for line in lines if line.startswith("OWNED_D3D12_NGX_ALPHA_PIXELS")]) != 2:
            failures.append("NGX alpha requires initial and closed-list replay GPU pixels exactly once")
        for row in values:
            _, lr, lg, lb, la, rr, rg, rb, ra, error = row
            expected = (112, 96, 80)
            actual_error = max(abs(value - wanted) for rgb in ((lr, lg, lb), (rr, rg, rb))
                               for value, wanted in zip(rgb, expected))
            if any(value > 255 for value in row[1:9]) or la > 96 or not 144 <= ra <= 240 or error != actual_error or error > 8:
                failures.append("NGX alpha actual RGB/alpha contrast or RGB preservation oracle failed")
        expected_case = "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_alpha flag128=1 nonopaque_contrast=1 rgb_preserved=1"
        if ([line for line in lines if line.startswith("OWNED_D3D12_NGX_FEATURE_CASE_PASS")] != [expected_case]
                or sum(line.startswith(prefix + "_PASS") for line in lines) != 1
                or lines.count("OWNED_D3D12_NGX_ALPHA_PASS cases=1 release_before_execute=1 replay=1 rgb_alpha=gpu_readback") != 1):
            failures.append("NGX focused alpha requires only its existing public alpha case and summary")
        return failures
    elif suite == "ngx-fg":
        expected = [
            "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=exact_capabilities fg=1 rr=1 non_sr_other=0",
            "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_fg count_max=1 unsupported_mfg=1 disable_buffer=unsupported reset=native_history release_before_execute=1 replay=native_encode",
        ]
        if ([line for line in lines if line.startswith("OWNED_D3D12_NGX_FEATURE_CASE_PASS")] != expected
                or sum(line.startswith(prefix + "_PASS") for line in lines) != 1
                or lines.count("OWNED_D3D12_NGX_FG_PASS capacity=1 native_output=1 unsupported_mfg=1") != 1
                or any(line.startswith("OWNED_D3D12_NGX_FEATURES_PASS") for line in lines)):
            failures.append("NGX focused FG requires its public capabilities, indexed-output case and exact summary")
        return failures
    elif suite == "ngx-features":
        expected = [
            "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=exact_capabilities fg=1 rr=1 non_sr_other=0",
            "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_fg count_max=1 unsupported_mfg=1 disable_buffer=unsupported reset=native_history release_before_execute=1 replay=native_encode",
            *[f"OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_rr variant={v} frames=8 handle13=1 denoising_guides=1 matrices_snapshotted=1 release_before_execute=1 replay=1 oracle_artifacts=8" for v in range(4)],
            "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_alpha flag128=1 nonopaque_contrast=1 rgb_preserved=1",
        ]
        if [line for line in lines if line.startswith("OWNED_D3D12_NGX_FEATURE_CASE_PASS")] != expected or lines.count("OWNED_D3D12_NGX_FEATURES_PASS") != 1:
            failures.append("NGX public feature inventory, four RR variants, FG or alpha protocol incomplete")
        return failures
    elif suite == "dxgi-factory":
        adapters = re.findall(r"^DXGI_FACTORY_ADAPTER preference=([012]) index=(\d+) luid=([0-9a-f]{8}:[0-9a-f]{8})$", stdout, re.MULTILINE)
        groups = [[(int(index), luid) for p, index, luid in adapters if p == str(preference)] for preference in range(3)]
        if not groups[0] or any([index for index, _ in group] != list(range(len(group))) or len({luid for _, luid in group}) != len(group) or {luid for _, luid in group} != {luid for _, luid in groups[0]} for group in groups) or [int(p) for p, _, _ in adapters] != sorted(int(p) for p, _, _ in adapters):
            failures.append("DXGI real adapter preference/LUID enumeration incomplete")
        if len([line for line in lines if line.startswith("DXGI_FACTORY_ADAPTER")]) != len(adapters) or lines.count("DXGI_FACTORY_EVENTS_OK identity preference nodes occlusion stereo lifetime") != 1:
            failures.append("DXGI factory lifetime/status proof missing")
        return failures
    else:
        vblank = re.findall(r"^OWNED_DXGI_OUTPUT_VBLANK waits=3 elapsed_qpc=(\d+) frequency=(\d+)$", stdout, re.MULTILINE)
        stats = re.findall(r"^OWNED_DXGI_OUTPUT_STATISTICS present=(\d+) present_refresh=(\d+) sync_refresh=(\d+) qpc=(\d+)$", stdout, re.MULTILINE)
        if len(vblank) != 1 or any(int(v) <= 0 for row in vblank for v in row) or len(stats) != 1 or any(int(v) <= 0 for row in stats for v in row):
            failures.append("DXGI native vblank/output statistics proof missing")
        expected = [
            "OWNED_DXGI_FULLSCREEN_FRAME api=d3d12 pixels=3888 exact=1 fence=complete present=ok",
            "OWNED_DXGI_FULLSCREEN_TRANSITION api=d3d12 enter=1 resize=1 frame=1 leave=1 rect=restored",
            "OWNED_DXGI_FULLSCREEN_REPEAT api=d3d12 cycles=2 restored=1",
            "OWNED_DXGI_FULLSCREEN_ERRORS api=d3d12 invalid=1 qi=1 desc=1 mode=1 resize=1 reentrant=1 native_enter=1 native_leave=1 preserved=1",
            "OWNED_DXGI_FULLSCREEN_CREATION api=d3d12 native=1 windowed=1 rect=restored",
            "OWNED_DXGI_FULLSCREEN_INVALID_CREATION api=d3d12 rejected=1 null_output=1",
            "OWNED_DXGI_OUTPUT_STATISTICS_PASS native=1 resize_disjoint=1 leave_unregistered=1",
            "OWNED_DXGI_FULLSCREEN_PASS",
        ]
        if any(lines.count(line) != 1 for line in expected) or any("api=d3d11" in line for line in lines):
            failures.append("DXGI D3D12 fullscreen transition/statistics matrix incomplete")
        return failures
    if len([line for line in lines if line.startswith(prefix + "_CASE_PASS")]) != case_count or len([line for line in lines if line.startswith(prefix + "_PASS")]) != 1:
        failures.append("malformed or extra game fixture evidence")
    return failures


RR_ORACLE_FILES = tuple(f"{prefix}-{frame}.bin" for prefix in (
    "native-rr-frame", "native-rr-transposed", "native-rr-mirror", "native-rr-mirror-transposed")
    for frame in range(1, 9)) + ("native-rr-replay-8.bin", "native-rr-mirror-replay-8.bin") + tuple(
    f"native-rr-{variant}input-{frame}.inputs" for variant in ("", "mirror-") for frame in range(1, 9))


def rr_oracle_comparison(frozen: Path, oracle: Path) -> dict:
    """Compare finite RGB half values exactly; no unmeasured tolerance allowance."""
    failures = []
    metrics = []
    input_metrics = []
    images = {}
    public_names = [f"public-rr-{variant}{suffix}.bin" for variant in ("", "linear-", "packed-", "mirror-")
                    for suffix in (*[f"frame-{frame}" for frame in range(1, 9)], "replay")]
    if sorted(path.name for path in frozen.glob("public-rr-*.bin")) != sorted(public_names):
        failures.append("RR public 32 history frames and four replay artifacts incomplete or extra")
    for root, names in ((frozen, public_names), (oracle, [name for name in RR_ORACLE_FILES if name.endswith(".bin")])):
        for name in names:
            path = root / name
            if path.is_symlink() or not path.is_file() or path.stat().st_size != 128 * 128 * 8:
                failures.append(f"RR artifact is not a regular exact-size RGBA16F image: {name}")
                continue
            values = struct.unpack("<65536e", path.read_bytes())
            if not all(math.isfinite(value) for value in values):
                failures.append(f"RR nonfinite half output: {name}")
                continue
            images[name] = values
    for variant in ("", "mirror-"):
        for frame in range(1, 9):
            public = frozen / f"public-rr-{variant}input-{frame}.inputs"
            native = oracle / f"native-rr-{variant}input-{frame}.inputs"
            if any(path.is_symlink() or not path.is_file() or path.stat().st_size != 128 * 128 * 48 + 140
                   for path in (public, native)):
                failures.append(f"RR producer input artifact missing or malformed: {public.name}")
                continue
            public_hash, native_hash = digest(public), digest(native)
            input_metrics.append({"public": public.name, "oracle": native.name,
                                  "public_sha256": public_hash, "oracle_sha256": native_hash})
            if public_hash != native_hash:
                failures.append(f"RR uploaded input bytes or matrix/jitter/reset differs: {public.name}")
    sensitive = 0
    for frame in range(1, 9):
        a, b = f"native-rr-mirror-{frame}.bin", f"native-rr-mirror-transposed-{frame}.bin"
        if a in images and b in images:
            sensitive += any(x != y for i, (x, y) in enumerate(zip(images[a], images[b])) if i % 4 != 3)
    if not sensitive:
        failures.append("RR independent mirror matrix negative control is insensitive")
    for variant in ("", "linear-", "packed-", "mirror-"):
        for frame in range(1, 10):
            name = f"public-rr-{variant}{'replay' if frame == 9 else f'frame-{frame}'}.bin"
            reference = (f"native-rr-{'mirror-' if variant == 'mirror-' else ''}replay-8.bin" if frame == 9 else
                         f"native-rr-{'mirror' if variant == 'mirror-' else 'frame'}-{frame}.bin")
            if name not in images or reference not in images:
                continue
            errors = [abs(x - y) for i, (x, y) in enumerate(zip(images[name], images[reference])) if i % 4 != 3]
            maximum, mean = max(errors), sum(errors) / len(errors)
            metrics.append({"public": name, "oracle": reference, "rgb_components": len(errors),
                            "max_abs_error": maximum, "mean_abs_error": mean,
                            "public_sha256": digest(frozen / name), "oracle_sha256": digest(oracle / reference)})
            if maximum != 0:
                failures.append(f"RR RGB differs from independent native oracle: {name}")
    return {"metrics": metrics, "input_metrics": input_metrics, "mirror_matrix_sensitive_frames": sensitive,
            "comparison": "finite-half-rgb-exact", "failures": failures}


def owned_core_load_evidence(stderr: str, runtime: Path, manifest: dict, arch: dict, frozen: Path, agility_inputs: dict | None = None, *, require_load: bool = False) -> dict:
    """Admit only current same-generation Core images at explicitly owned paths."""
    rows = re.findall(r'Loaded L"([^"\n]*d3d12core\.dll)"[^\n]*(native|builtin)\b', stderr, re.IGNORECASE)
    sdk = manifest.get("agility_sdk", {})
    if sdk.get("delivery") != "native-only":
        # Genuine retained runtimes used builtin Core or had no Core. Native
        # images are never silently admitted by that historical contract.
        return {"loads": [], "foreign_loads": [path for path, mode in rows if mode.lower() == "native"],
                "failures": [], "contract": "retained-non-native-Core"}
    if not rows:
        return {"loads": [], "foreign_loads": [], "contract": "native-only",
                "failures": ["native owned Core was not observed in actual Wine module-load evidence"] if require_load else []}
    directory = "lib/wine/i386-windows" if arch["guest_arch"] == "x86" else arch["windows_directory"]
    relative = str(Path(directory) / "d3d12core.dll")
    record = sdk.get("modules", {}).get(relative)
    if not isinstance(record, dict):
        return {"loads": [], "foreign_loads": [path for path, _ in rows],
                "failures": ["native Core source manifest identity missing"], "contract": "native-only"}
    candidates = [runtime / relative]
    if agility_inputs is not None:
        candidates += [frozen / "sdk-owned/d3d12core.dll", frozen / "sdk-alternate/d3d12core.dll"]
    admitted = {}
    failures = []
    abi = "x86" if arch["guest_arch"] == "x86" else arch["windows_abi"]
    for image in candidates:
        try:
            if image.is_symlink() or not image.is_file() or digest(image) != record["sha256"] or inspect_pe(image)["windows_abi"] != abi:
                raise ValueError(f"owned Core path/image/ABI changed: {image}")
            admitted[("Z:" + str(image).replace("/", "\\")).casefold()] = str(image)
        except (OSError, ValueError) as error:
            failures.append(str(error))
    loads, foreign = [], []
    observed_hashes = {}
    for path, mode in rows:
        decoded = path.replace("\\\\", "\\").replace("/", "\\")
        normalized = decoded.casefold()
        if mode.lower() != "native" or normalized not in admitted:
            foreign.append(path)
        else:
            # Windows path matching is case-insensitive; hash the actual named
            # image too, so a case-sensitive host cannot hide another image.
            try:
                if decoded not in observed_hashes:
                    observed_hashes[decoded] = digest(Path(decoded[2:].replace("\\", "/")))
                if observed_hashes[decoded] != record["sha256"]:
                    foreign.append(path)
                    continue
                loads.append({"path": admitted[normalized], "sha256": record["sha256"], "generation": record["generation"], "abi": abi, "mode": "native"})
            except OSError as error:
                foreign.append(path)
                failures.append(str(error))
    if agility_inputs is not None and {row["path"] for row in loads} != {str(path) for path in candidates}:
        failures.append("Agility requires actual native loads of installed, explicit and alternate owned Core images")
    return {"loads": loads, "foreign_loads": foreign, "failures": failures, "contract": "native-only"}


def agility_failures(stdout: str, stderr: str, core_path: Path | None = None, generation: str | None = None, installed_core_path: Path | None = None) -> list[str]:
    prefix = SUITE_PREFIXES["agility"]
    failures = []
    cases = re.findall(r"^" + prefix + r"_CASE_PASS case=([a-z_]+)$", stdout, re.MULTILINE)
    if cases != ["rejected_modules", "selected_core_path_version", "factory_configuration_gpu",
                 "retained_objects_gpu", "unused_core_unload", "set_sdk_version_fresh_process"] or stdout.splitlines().count(prefix + "_PASS cases=6") != 1:
        failures.append("Agility requires all six public loader/lifetime/GPU contracts exactly once")
    rejects = re.findall(r"^" + prefix + r"_REJECT case=([a-z_]+) output=null loaded=0 hr=0x([0-9a-f]{8})$", stdout, re.MULTILINE)
    if [name for name, _ in rejects] != ["wrong_version", "missing_path", "missing_file", "foreign_module", "wrong_abi", "different_generation"] or any(int(hr, 16) < 0x80000000 for _, hr in rejects):
        failures.append("Agility must clear outputs and not load every rejected Core")
    gpu = re.findall(r"^" + prefix + r"_GPU phase=([a-z_]+) expected=([0-9a-f]{8}) actual=([0-9a-f]{8}) sdk=618$", stdout, re.MULTILINE)
    expected_gpu = [(phase, f"a618{index:04x}", f"a618{index:04x}") for phase, index in
                    (("configured", 1), ("retained", 2), ("children", 3), ("set_sdk_lower_child", 9),
                     ("set_sdk_before_late", 4), ("set_sdk_child", 5), ("apply_global_child", 6),
                     ("initialized_alternate_child", 7), ("executable_lower_child", 10), ("executable_exports_child", 8))]
    if gpu != expected_gpu:
        failures.append("Agility requires exact configured, retained, child-only, SetSDKVersion and retail Apply GPU readbacks")
    cores = re.findall(r"^" + prefix + r"_CORE sdk=618 generation=([0-9a-f]{32}) path=(.+) cwd_base=exe source=staged-copy$", stdout, re.MULTILINE)
    unloads = re.findall(r"^" + prefix + r"_UNLOAD path=(.+) live_objects=0 loaded=0$", stdout, re.MULTILINE)
    if len(cores) != 1 or len(unloads) != 1 or (cores and unloads and cores[0][1] != unloads[0]):
        failures.append("Agility requires actual selected Core identity and matching unused-module unload")
    elif core_path is not None and cores[0][1].casefold() != ("Z:" + str(core_path).replace("/", "\\")).casefold():
        failures.append("Agility selected a Core outside the frozen EXE-relative SDK directory")
    if generation is not None and (not cores or cores[0][0] != generation):
        failures.append("Agility selected a different staged Core generation")
    if stdout.splitlines().count(prefix + "_DIRECT_CORE_FACTORY sdk=618 selected_module=retained owners=1 final_unload=1") != 1:
        failures.append("Agility requires the actual Core public factory to retain the selected module as sole owner and release it on final destruction")
    if stdout.splitlines().count(prefix + "_CONCURRENT_FREE rounds=32 factories=32 configurations=32 sdk=618 cold_start=1 module=stable final_unload=1") != 1:
        failures.append("Agility requires bounded concurrent cold factory/configuration creation and FreeUnusedSDKs with live identity and actual final unload")
    inbox = re.findall(r"^" + prefix + r"_INBOX_SELECTION operation=([a-z_]+) requested=(\d+) exe_sdk=(\d+) sdk=618 generation=([0-9a-f]{32}) path=(.+) app_loaded=0 cwd_base=exe$", stdout, re.MULTILINE)
    if [(operation, requested, exe_sdk) for operation, requested, exe_sdk, _, _ in inbox] != [
            ("set_sdk", "617", "619"), ("set_sdk", "618", "619"),
            ("exe_exports", "617", "617"), ("exe_exports", "618", "618")]:
        failures.append("Agility requires older/equal EXE and setter inbox precedence, with setters overriding immutable EXE619")
    if installed_core_path is None or any(path.casefold() != ("Z:" + str(installed_core_path).replace("/", "\\")).casefold() or
            (generation is not None and selected_generation != generation) for _, _, _, selected_generation, path in inbox):
        failures.append("Agility inbox selections must identify the actual installed same-generation Core, not an app-local copy")
    newer = re.findall(r"^" + prefix + r"_EXECUTABLE_REJECT requested=619 output=null app_loaded=0 hr=0x([0-9a-f]{8})$", stdout, re.MULTILINE)
    setter_rejects = re.findall(r"^" + prefix + r"_SET_SDK_REJECT requested=619 selection=UNCHANGED app_loaded=0 hr=0x([0-9a-f]{8})$", stdout, re.MULTILINE)
    if len(newer) != 1 or len(setter_rejects) != 2 or any(int(hr, 16) < 0x80000000 for hr in newer + setter_rejects):
        failures.append("Agility newer global requests must fail without foreign loads, stale outputs or setter selection mutation")
    developer_modes = re.findall(r"^" + prefix + r"_DEVELOPER mode=([a-z_]+) ordinary_set=DENIED ordinary_sdk1=AVAILABLE source=os-registry$", stdout, re.MULTILINE)
    if developer_modes != ["set_sdk_lower", "set_sdk", "apply_global"]:
        failures.append("Agility must distinguish ordinary-mode SDK1 factories from Developer-only SetSDKVersion")
    failed_operations = re.findall(r"^" + prefix + r"_HRESULT operation=([a-z_]+) hr=0x([0-9a-f]{8})$", stdout, re.MULTILINE)
    if [name for name, _ in failed_operations] != ["ordinary_set", "ordinary_set", "late_set", "late_global_removal", "ordinary_set", "alternate_apply"] or any(int(hr, 16) < 0x80000000 for _, hr in failed_operations):
        failures.append("Agility requires actual failing HRESULTs without pinning incidental error codes")
    if any(name == "alternate_apply" and hr != "887e0003" for name, hr in failed_operations):
        failures.append("Agility alternate-Core Apply must return the specified D3D12_ERROR_INVALID_REDIST")
    if stdout.splitlines().count(prefix + "_SET_SDK before_device=PASS late_call=FAIL existing_global=REMOVED independent_device=LIVE selection=UNCHANGED os_state=RESTORED sdk=618") != 1:
        failures.append("Agility requires fresh-process SetSDKVersion success and late-call failure")
    if stdout.splitlines().count(prefix + "_SET_SDK_LOWER before_device=PASS exe_sdk=619 os_state=RESTORED sdk=618") != 1:
        failures.append("Agility requires older SetSDKVersion to override EXE619 and restore OS state")
    if stdout.splitlines().count(prefix + "_APPLY_GLOBAL ordinary_mode=PASS sdk=618 selected_core=RETAINED initialized_factory=PASS alternate_apply=REJECTED global_device=LIVE independent_device=LIVE os_state=RESTORED") != 1:
        failures.append("Agility requires retail ApplyToGlobalState Core selection, retention and InitializeFromGlobalState")
    if "_FAIL" in stdout + stderr or "_MISMATCH" in stdout + stderr:
        failures.append("Agility failure marker")
    return failures


def prepare_agility_inputs(runtime: Path, manifest: dict, arch: dict, frozen: Path) -> dict:
    sdk = manifest.get("agility_sdk", {})
    modules = sdk.get("modules", {})
    windows_directory = "lib/wine/i386-windows" if arch["guest_arch"] == "x86" else arch["windows_directory"]
    relative = str(Path(windows_directory) / "d3d12core.dll")
    opposite = "lib/wine/i386-windows/d3d12core.dll" if arch["guest_arch"] == "x64" else "lib/wine/x86_64-windows/d3d12core.dll"
    if sdk.get("ownership") != "self-owned" or sdk.get("version") != 618 or relative not in modules or opposite not in modules:
        raise ValueError("--suite agility requires a same-generation paired owned SDK618 Core runtime")
    source = runtime / relative
    generation = modules[relative]["generation"]
    abi = "x86" if arch["guest_arch"] == "x86" else arch["windows_abi"]
    if source.is_symlink() or not source.is_file() or digest(source) != modules[relative]["sha256"] or inspect_pe(source)["windows_abi"] != abi:
        raise ValueError("staged Core source identity changed before freezing")
    for name in ("sdk-owned", "sdk-alternate", "sdk-empty", "sdk-foreign", "sdk-wrong-abi", "sdk-other-generation", "other-cwd"):
        (frozen / name).mkdir()
    shutil.copy2(source, frozen / "sdk-owned/d3d12core.dll")
    shutil.copy2(source, frozen / "sdk-alternate/d3d12core.dll")
    shutil.copy2(runtime / opposite, frozen / "sdk-wrong-abi/d3d12core.dll")
    shutil.copy2(runtime / windows_directory / "dxgi.dll", frozen / "sdk-foreign/d3d12core.dll")
    payload = source.read_bytes()
    old = generation.encode() + b"\0"
    if old not in payload:
        raise ValueError("staged Core generation DATA missing from actual source image")
    changed = ("0" if generation[0] != "0" else "1") + generation[1:]
    (frozen / "sdk-other-generation/d3d12core.dll").write_bytes(payload.replace(old, changed.encode() + b"\0"))
    return {"version": 618, "generation": generation, "source": relative, "source_sha256": digest(source),
            "inputs": {str(path.relative_to(frozen)): {"sha256": digest(path), "size": path.stat().st_size}
                       for path in sorted(frozen.glob("sdk-*/d3d12core.dll"))},
            "negative_inputs": {"foreign_module": "owned DXGI image, not a Core", "wrong_abi": opposite,
                                "different_generation": "actual staged Core with generation DATA changed"}}


def depth_bounds_failures(stdout: str, stderr: str) -> list[str]:
    prefix = SUITE_PREFIXES["depth-bounds"]
    failures = []
    expected = []
    for name in DEPTH_BOUNDS_CASES:
        samples = 4 if name.startswith("sample4_") else 1
        fmt = 55 if "d16" in name else 45 if "d24" in name or name in ("inverted_late_uav", "inverted_early_uav", "readonly_stencil", "early_discard_commits_ds", "late_discard_preserves_ds") else 40
        expected.append(f"{prefix}_CASE_PASS case={name} samples={samples} format={fmt} "
                        f"cells={16 * samples} queries=3 replay={2 if name == 'replay' else 1}")
    rows = [line for line in stdout.splitlines() if line.startswith(prefix + "_CASE_PASS")]
    if rows != expected:
        failures.append("depth bounds exact GPU color/depth/stencil/UAV/query case matrix incomplete")
    stats = re.findall(r"^" + prefix + r"_STATS case=([\w_]+) replay=(\d+) ps=\d+ occlusion=\d+ binary=[01]$", stdout, re.MULTILINE)
    wanted_stats = [(name, str(replay)) for name in DEPTH_BOUNDS_CASES
                    for replay in range(2 if name == "replay" else 1)]
    if stats != wanted_stats:
        failures.append("depth bounds query evidence incomplete")
    caps = [line for line in stdout.splitlines() if line.startswith(prefix + "_CAP")]
    terminals = [line for line in stdout.splitlines() if line.startswith(prefix + "_PASS")]
    if caps != [prefix + "_CAP supported=1"] or terminals != [prefix + "_PASS cases=50"]:
        failures.append("depth bounds supported capability or terminal proof missing")
    if prefix + "_FAIL" in stdout + stderr or prefix + "_MISMATCH" in stdout + stderr:
        failures.append("depth bounds fixture reported a GPU mismatch")
    return failures


def conservative_raster_failures(stdout: str, stderr: str) -> list[str]:
    """Strict public GPU protocol; provisional tiers cannot publish a Tier3 pass."""
    prefix = "OWNED_D3D12_CONSERVATIVE_RASTER"
    paths = CONSERVATIVE_RASTER_PATHS
    failures = []
    cases = re.findall(r"^" + prefix + r"_CASE_PASS (\w+)$", stdout, re.MULTILINE)
    expected_cases = ["ia_capture_parameter_integrity", "pso_validation", "spec_bring_up", "spec_half_pixel",
                      "spec_tir_forced4_coverage_0xf", "spec_msaa4_coverage_0xf_samplemask_0x5"]
    expected_cases += [f"oracle_{path}" for path in paths] + ["cull_back", "depth_extrapolated_per_sample_clamped",
        "tier3_exact_grid_all_families", "tier3_large_coordinates", "tier3_degenerate_winding_cull_vertex0",
        "tier3_mesh_degenerate_winding_cull_vertex0", "tier3_depth_clip_degenerate_near_far",
        "tier3_inner_samplemask_forced4_dxbc_dxil", "tier3_off_geometry_on_inner_toggle", "tier3_helper_inner_zero",
        "tier3_early_late_depth_stencil_uav_queries"]
    if cases != expected_cases:
        failures.append("missing, duplicate or reordered conservative numeric cases")
    oracle = re.findall(r"^" + prefix + r"_ORACLE path=(\w+) samples=([18]) triangles=64 covered=(\d+) ambiguous=\d+ invocations=\d+$", stdout, re.MULTILINE)
    distances = re.findall(r"^" + prefix + r"_DISTANCE path=(\w+) samples=([18]) covered=\d+ invocations=\d+$", stdout, re.MULTILINE)
    interpolation = re.findall(r"^" + prefix + r"_DISTANCE_INTERPOLATION path=(\w+) samples=8 indirect=1 mask=2 invocations=\d+$", stdout, re.MULTILINE)
    views = re.findall(r"^" + prefix + r"_VIEW path=(\w+) mask=([023]) samples=8 invocations=\d+$", stdout, re.MULTILINE)
    combinations = re.findall(r"^" + prefix + r"_COMBINATION path=(\w+) indirect=([01]) samples=8 .*", stdout, re.MULTILINE)
    if ([(path, sample) for path, sample, _ in oracle] != [(path, sample) for path in paths for sample in ("1", "8")]
            or any(int(covered) <= 100 for _, _, covered in oracle)
            or distances != [(path, sample) for path in paths for sample in ("1", "8")]
            or interpolation != list(paths)
            or views != [(path, mask) for path in paths for mask in ("0", "2", "3")]
            or combinations != [(path, indirect) for path in paths for indirect in ("0", "1")]
            or not re.search(r"^" + prefix + r"_DEPTH samples=4 checked=\d+ clamped=[1-9]\d* covered=\d+ max_error=[\d.e+-]+$", stdout, re.MULTILINE)):
        failures.append("incomplete retained geometry/depth numeric proof")
    grid = re.findall(r"^" + prefix + r"_TIER3 path=(\w+) case=(\w+) samples=([14]) covered=(\d+) mandatory=(\d+) inner=(\d+) edge=(\d+) uncertain=(\d+)$", stdout, re.MULTILINE)
    grid_cases = (("interior_direct", "1"), ("outer_gap_indirect", "4"), ("inner_boundary_indexed_mesh", "4"),
                  ("point_direct", "1"), ("line_indirect_indexed_mesh", "4"), ("corner_bundle", "1"), ("interior_replay", "4"))
    if ([(path, case, sample) for path, case, sample, *_ in grid] !=
            [(path, case, sample) for path in paths for case, sample in grid_cases]
            or sum(line.startswith(prefix + "_TIER3 path=") for line in stdout.splitlines()) != len(grid)):
        failures.append("incomplete or duplicate exact-grid Tier3 family/route/sample proof")
    for _, case, _, covered, mandatory, inner, edge, uncertain in grid:
        c, m, i, e, u = map(int, (covered, mandatory, inner, edge, uncertain))
        degenerate = case in ("point_direct", "line_indirect_indexed_mesh", "corner_bundle")
        if (not 0 < c <= 1024 or not 0 <= m <= c or i + e + u != c
                or (degenerate and (i or u or e != c))
                or (case == "point_direct" and (c != 1 or m != 1))
                or (case == "line_indirect_indexed_mesh" and (c != 21 or m != 21))
                or (case == "corner_bundle" and (c > 4 or m != 0))
                or (not degenerate and (i == 0 or e == 0))
                or (case == "inner_boundary_indexed_mesh" and u == 0)):
            failures.append("invalid exact-grid covered/interior/edge/uncertainty totals")
    large = re.findall(r"^" + prefix + r"_TIER3_LARGE viewport=-16384,32768 covered=(\d+) inner=(\d+) edge=(\d+)$", stdout, re.MULTILINE)
    if len(large) != 1 or not (0 < int(large[0][1]) < int(large[0][0]) <= 1024 and int(large[0][2]) > 0):
        failures.append("missing large-coordinate cancellation boundary proof")
    depth_clip = re.findall(r"^" + prefix + r"_DEPTH_CLIP path=vs_dxil plane=(near|far) enabled=([01]) covered=(\d+) mandatory=(\d+) "
                            r"probe_clip=([0-9a-f]{8}) expected_clip=([0-9a-f]{8}) probe_depth=([0-9a-f]{8}) expected_depth=([0-9a-f]{8})$", stdout, re.MULTILINE)
    if [(plane, enabled) for plane, enabled, *_ in depth_clip] != [(plane, enabled) for plane in ("near", "far") for enabled in ("1", "0")]:
        failures.append("incomplete near/far captured depth-clipping proof")
    for plane, enabled, covered, mandatory, actual_clip, expected_clip, actual_depth, expected_depth in depth_clip:
        c, m = int(covered), int(mandatory)
        clip = "3e066666" if enabled == "1" else "3f31999a"
        depth = ("00000000" if plane == "near" else "3f800000") if enabled == "1" else ("39000000" if plane == "near" else "3f7ff800")
        if (actual_clip != clip or expected_clip != clip or actual_depth != depth or expected_depth != depth
                or (enabled == "1" and (m != 10 or not 10 <= c <= 28))
                or (enabled == "0" and (c != 64 or m != 64))):
            failures.append("post-depth-clip Vertex0 or ordinary in-range attribute/depth mismatch")
    interaction = re.findall(r"^" + prefix + r"_INTERACTION producer=(dxil|dxbc) case=(\w+) uav=(\d+) occlusion=(\d+) accepted=([01]) inner=(\d+)$", stdout, re.MULTILINE)
    interaction_cases = ("early_depth_pass", "early_depth_fail", "late_depth_pass_output", "late_depth_fail_uav",
                         "early_stencil_pass", "early_stencil_fail", "late_stencil_pass", "late_stencil_fail_uav", "late_stencil_replace")
    if [(producer, case) for producer, case, *_ in interaction] != [(producer, case) for producer in ("dxil", "dxbc") for case in interaction_cases]:
        failures.append("incomplete early/late UAV/depth/stencil/query proof")
    for _, case, uav, occlusion, accepted, inner in interaction:
        u, q, i = map(int, (uav, occlusion, inner))
        passed = "fail" not in case
        suppressed = case.startswith("early_") and not passed
        if (int(accepted) != int(passed) or q != (4 * u if passed else 0)
                or (suppressed and (u or i)) or (not suppressed and not 0 < i < u <= 1024)):
            failures.append("early/late side-effect or sample-count mismatch")
    lines = stdout.splitlines()
    if (re.findall(r"^" + prefix + r"_CAPS tier=(\d+)$", stdout, re.MULTILINE) != ["3"]
            or not lines or lines[-1] != prefix + "_PASS paths=10 rejected=0 tier=3"
            or lines.count(prefix + "_PASS paths=10 rejected=0 tier=3") != 1
            or prefix + "_PATH_REJECTED" in stdout or prefix + "_FAIL" in stdout + stderr):
        failures.append("Tier3 public capability/terminal/failure contract")
    return failures


def diagnostics_failures(stdout: str, stderr: str) -> list[str]:
    prefix = SUITE_PREFIXES["diagnostics"]
    cases = re.findall(r"^" + prefix + r"_CASE_PASS case=([A-Za-z_]+)$", stdout, re.MULTILINE)
    expected = ["agility_installed_factory_gpu_isolation", "validation", "storage_retrieval",
                "filters_callbacks", "native_markers_dred_removal", "sdk_tools_contracts"]
    failures = []
    if cases != expected or stdout.splitlines().count(prefix + "_PASS cases=6") != 1:
        failures.append("diagnostics requires all six public GPU/configuration contracts exactly once")
    if "_FAIL" in stdout + stderr or "_MISMATCH" in stdout + stderr:
        failures.append("diagnostics failure marker")
    return failures


def suite_inputs(suite: str, fixture: Path, dxil_dir: Path) -> list[Path]:
    """Inputs a suite reads beside its own executable, frozen before launch."""
    if suite in ("ngx-fg-present", "ngx-alpha", "ngx-fg"):
        return suite_inputs("ngx-features", fixture, dxil_dir)
    game_sources = {
        "sr-alpha": ("owned-d3d12-sr-alpha.cpp", "owned-d3d12-temporal.cpp"),
        "fsr-debug": ("owned-d3d12-fsr-debug.cpp", "owned-d3d12-fsr.cpp", "owned-d3d12-temporal.cpp"),
        "ngx-features": ("owned-d3d12-ngx-features.cpp", "owned-d3d12-ngx.cpp",
                         "owned-d3d12-temporal.cpp", "owned-rr-pattern.h",
                         "owned-rr-native-control.mm", "owned-d3d12-ngx-msvc.cpp",
                         "owned-d3d12-ngx-present.inc", "owned-fg-present.h"),
        "dxgi-factory": ("owned-dxgi-factory-events.cpp",),
        "dxgi-fullscreen": ("owned-dxgi-fullscreen.cpp",),
        "corewindow-composition": ("owned-d3d12-corewindow-composition.cpp", "owned-d3d12-composition.inc"),
        "composition": ("owned-d3d12-corewindow-composition.cpp", "owned-d3d12-composition.inc"),
        "diagnostics": ("owned-d3d12-diagnostics.cpp",),
        "agility": ("owned-d3d12-agility.cpp",),
    }
    if suite in game_sources:
        inputs = [REPOSITORY_ROOT / "graphics-translator/tests" / name for name in game_sources[suite]]
        if suite == "agility":
            inputs += [fixture.with_name(fixture.stem + f"-executable-{version}.exe") for version in (617, 618, 619)]
        if suite == "ngx-features":
            inputs += [REPOSITORY_ROOT / "external/nvngx/include" / name for name in
                       ("nvsdk_ngx.h", "nvsdk_ngx_defs.h", "nvsdk_ngx_params.h")]
        if suite == "fsr-debug":
            inputs += [REPOSITORY_ROOT / "src/fsr/include" / name for name in
                       ("upscalers/include/ffx_upscale.h", "api/include/ffx_api.h",
                        "api/include/ffx_api_types.h", "api/include/dx12/ffx_api_dx12.h")]
        return inputs
    if suite == "nvapi-reflex":
        return [REPOSITORY_ROOT / "graphics-translator/tests/owned-nvapi-reflex.cpp"]
    if suite == "queue-priority":
        return [REPOSITORY_ROOT / "graphics-translator/tests/owned-d3d12-queue-priority.cpp",
                REPOSITORY_ROOT / "src/d3d12/d3d12_interfaces_ext.hpp"]
    if suite.startswith("relaxed-casting"):
        return [REPOSITORY_ROOT / "graphics-translator/tests/owned-d3d12-relaxed-casting.cpp"]
    if suite in ("matrix-capabilities", "wave-mma-capabilities"):
        inputs = [REPOSITORY_ROOT / "graphics-translator/tests/owned-d3d12-matrix-capabilities.cpp"]
        if suite == "wave-mma-capabilities":
            return inputs + [dxil_dir / "original-d3d12.h"]
        return inputs + suite_inputs("matrix-runtime", fixture, dxil_dir)
    if suite == "matrix-runtime":
        return [REPOSITORY_ROOT / "graphics-translator/tests/owned-d3d12-matrix-runtime.cpp",
                REPOSITORY_ROOT / "src/d3d12/d3d12_matrix.hpp",
                REPOSITORY_ROOT / "src/d3d12/d3d12_interfaces_ext.hpp",
                dxil_dir / "preview-d3d12.h", dxil_dir / "pinned-v1-runtime-spec.txt"]
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
    if suite == "advanced-texture":
        return [dxil_dir / name for name in ADVANCED_TEXTURE_SHADERS +
                ("advanced-texture.hlsl", "build-advanced-texture.sh")]
    if suite == "depth-casting":
        return [dxil_dir / name for name in
                ("depth-casting-16.dxil", "depth-casting-32.dxil", "depth-casting-msaa-vs.dxil",
                 "depth-casting-16-msaa-cs.dxil", "depth-casting-16-msaa-ps.dxil",
                 "depth-casting-32-msaa-cs.dxil", "depth-casting-32-msaa-ps.dxil",
                 "depth-casting.hlsl", "build-depth-casting.sh")]
    if suite == "depth-bounds":
        return [dxil_dir / name for name in DEPTH_BOUNDS_INPUTS]
    if suite == "dxil-resource-review":
        return [dxil_dir / name for name in DXIL_RESOURCE_REVIEW_SHADERS]
    if suite == "predication-indirect":
        return [dxil_dir / name for name in PREDICATION_INDIRECT_SHADERS]
    if suite in ("atomic-copy", "atomic-copy-live"):
        return [dxil_dir / name for name in ATOMIC_COPY_SHADERS]
    if suite == "atomic64-phase":
        return [dxil_dir / name for name in ATOMIC64_PHASE_INPUTS]
    if suite == "raytracing":
        return [dxil_dir / name for name in RAYTRACING_SHADERS]
    if suite == "raytracing-atomic64":
        return [dxil_dir / name for name in
                ("raytracing-contract.dxil", "raytracing-atomic64.dxil", "raytracing-atomic64.hlsl",
                 "build-raytracing-atomic64.sh")]
    if suite == "raytracing-omm":
        return [dxil_dir / name for name in RAYTRACING_OMM_SHADERS + ("raytracing-omm.hlsl", "build-raytracing-omm.sh")]
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
        return ([dxil_dir / f"sm6-{entry}.dxil" for entry in SM6_ENTRIES]
                + [dxil_dir / name for name in SM6_SOURCES + FP64_LEGACY_INPUTS])
    if suite == "fp64":
        return ([dxil_dir / f"sm6-{entry}.dxil" for entry in FP64_ENTRIES]
                + [dxil_dir / name for name in FP64_SOURCES + FP64_LEGACY_INPUTS])
    if suite == "waves":
        return [dxil_dir / f"sm6-{entry}.dxil" for entry in WAVE_ENTRIES] + [dxil_dir / name for name in SM6_SOURCES]
    if suite == "vectors":
        return ([dxil_dir / f"sm6-{entry}.dxil" for entry in VECTOR_ENTRIES]
                + [dxil_dir / name for name in VECTOR_SOURCES])
    if suite == "matrix-shaders":
        return ([dxil_dir / f"sm6-{entry}.dxil" for entry in MATRIX_ENTRIES]
                + [dxil_dir / name for name in MATRIX_SOURCES])
    if suite == "writable-msaa":
        source = Path(__file__).resolve().parents[1] / "graphics-translator/tests"
        return ([dxil_dir / f"sm6-{entry}.dxil" for entry in WRITABLE_MSAA_ENTRIES]
                + [source / "owned-d3d12-writable-msaa.hlsl", source / "build-writable-msaa.sh"])
    if suite == "mesh-shader":
        return ([dxil_dir / f"mesh-shader-{entry}.dxil" for entry in MESH_SHADER_ENTRIES]
                + [dxil_dir / name for name in MESH_SHADER_SOURCES])
    if suite == "atomic64-mesh-groupshared":
        source = REPOSITORY_ROOT / "graphics-translator/tests"
        return ([dxil_dir / f"mesh-shader-{entry}.dxil" for entry in
                 ("ms_groupshared64", "as_groupshared64", "ms_derivative_payload", "ps_color")]
                + [source / "dxil" / name for name in MESH_SHADER_SOURCES]
                + [source / name for name in ("owned-d3d12-mesh-shader.cpp", "owned-d3d12-early-depth-oracle.inc")])
    if suite == "view-instancing":
        return [dxil_dir / name for name in VIEW_INSTANCING_INPUTS]
    if suite == "atomic64-helpers":
        source = REPOSITORY_ROOT / "graphics-translator/tests"
        return ([dxil_dir / f"atomic64-helpers-{entry}.dxil" for entry in ("vs", "ps", "early", "sample", "inclusive")]
                + [source / "dxil" / name for name in ("atomic64-helpers.hlsl", "build-atomic64-helpers.sh")]
                + [source / name for name in ("owned-d3d12-atomic64-helpers.cpp", "owned-d3d12-sm6.cpp")])
    if suite == "atomic64-pixel-contention":
        source = REPOSITORY_ROOT / "graphics-translator/tests"
        return ([dxil_dir / f"atomic64-pixel-contention-{entry}.dxil" for entry in ("vs", "add", "cas")]
                + [dxil_dir / name for name in ("atomic64-pixel-contention-provenance.json",
                                               "atomic64-pixel-contention-dxc-version.txt")]
                + [source / "dxil" / name for name in ("atomic64-pixel-contention.hlsl", "build-atomic64-pixel-contention.sh")]
                + [source / name for name in ("owned-d3d12-atomic64-pixel.cpp", "owned-d3d12-sm6.cpp")])
    if suite == "atomic64-pixel-coupled":
        source = REPOSITORY_ROOT / "graphics-translator/tests"
        return ([dxil_dir / f"atomic64-pixel-r{rov}-t{typed}-s{sample}-e{early}.dxil"
                 for rov in range(2) for typed in range(2) for sample in range(2) for early in range(2)]
                + [dxil_dir / f"atomic64-pixel-{entry}.dxil" for entry in ("vs", "probe")]
                + [source / "dxil" / name for name in ("atomic64-pixel.hlsl", "build-atomic64-pixel.sh")]
                + [source / name for name in ("owned-d3d12-atomic64-pixel.cpp", "owned-d3d12-sm6.cpp")])
    if suite in ("atomic64-groupshared", "atomic64-groupshared-capacity"):
        entries = ("groupcapacity",) if suite.endswith("-capacity") else ("groupops", "groupstress")
        source = REPOSITORY_ROOT / "graphics-translator/tests"
        return ([dxil_dir / f"sm6-cs_atomic64_{entry}.dxil" for entry in entries]
                + [source / "dxil" / name for name in ("atomic64-groupshared.hlsl", "build-sm6.sh")]
                + [source / name for name in ("owned-d3d12-atomic64.cpp", "owned-d3d12-sm6.cpp")])
    if suite == "atomic64-typed":
        source = REPOSITORY_ROOT / "graphics-translator/tests"
        return ([dxil_dir / f"atomic64-typed-{shape}-{sign}-{entry}.dxil"
                 for shape in range(6) for sign in range(2) for entry in ("operations", "contention", "snapshot")]
                + [dxil_dir / name for name in ("atomic64-typed-0-0-aliases.dxil",
                   "atomic64-typed-format-0.dxil", "atomic64-typed-format-1.dxil",
                   "atomic64-typed-format-2.dxil", "atomic64-typed-float.dxil")]
                + [source / "dxil" / name for name in ("atomic64-typed.hlsl", "atomic64-typed-format.hlsl",
                   "atomic64-typed-float.hlsl", "atomic64-typed-msaa.hlsl", "build-atomic64-typed.sh")]
                + [source / name for name in ("owned-d3d12-atomic64-typed.cpp", "owned-d3d12-sm6.cpp")])
    if suite == "atomic64-producers":
        return ([dxil_dir / f"atomic64-producers-{entry}.dxil" for entry in ATOMIC64_PRODUCER_ENTRIES]
                + [dxil_dir / name for name in ("atomic64-producers.hlsl", "build-atomic64-producers.sh")]
                + [REPOSITORY_ROOT / "graphics-translator/tests" / name for name in
                   ("owned-d3d12-atomic64-producers.cpp", "owned-d3d12-sm6.cpp")])
    if suite == "work-graphs":
        return [dxil_dir / name for name in ("work-graphs.hlsl", "work-graphs-sharing.hlsl", "work-graphs-linkage.hlsl", "build-work-graphs.sh",
                                           "work-graphs-nodes.dxil", "work-graphs-sharing.dxil", "work-graphs-link-nodes.dxil", "work-graphs-link-helpers.dxil", "work-graphs-root-subobjects.dxil")] + [
                                               REPOSITORY_ROOT / "graphics-translator/tests/owned-d3d12-work-graphs.cpp",
                                               REPOSITORY_ROOT / "build/local/p3-20261003/matrix/preview-d3d12.h"]
    if suite == "workgraphs-atomic64":
        return [dxil_dir / name for name in ("atomic64-workgraph-records.dxil", "atomic64-workgraph-records.hlsl", "build-sm6.sh")] + [
            REPOSITORY_ROOT / "graphics-translator/tests/owned-d3d12-atomic64-workgraph.cpp",
            REPOSITORY_ROOT / "build/local/p3-20261003/matrix/preview-d3d12.h"]
    if suite == "workgraphs-atomic64-typed":
        return [dxil_dir / name for name in ("atomic64-workgraph-typed.dxil", "atomic64-workgraph-typed.hlsl", "build-sm6.sh")] + [
            REPOSITORY_ROOT / "graphics-translator/tests/owned-d3d12-atomic64-workgraph-typed.cpp",
            REPOSITORY_ROOT / "build/local/p3-20261003/matrix/preview-d3d12.h"]
    if suite == "vrs":
        return [dxil_dir / name for name in VRS_INPUTS]
    if suite == "sampler-feedback":
        return [dxil_dir / name for name in SAMPLER_FEEDBACK_INPUTS]
    if suite == "feature-level":
        return [dxil_dir / name for name in FEATURE_LEVEL_INPUTS]
    if suite == "options13":
        return [REPOSITORY_ROOT / "graphics-translator/tests" / name for name in
                ("owned-d3d12-options13.cpp", "owned-d3d12-render-p4.inc")]
    if suite == "conservative-raster":
        return [dxil_dir / name for name in CONSERVATIVE_RASTER_INPUTS]
    return []


def multiqueue_submission_failures(events: list[dict]) -> list[str]:
    # This fixture has 32 compute/copy pairs, 64 signals and 63 CPU-gated waits.
    expected = {"execute_list": 64, "signal_committed": 64, "wait": 63, "submit": 128, "complete": 128}
    counts = {kind: sum(event.get("event") == kind for event in events) for kind in expected}
    if counts != expected or any("submission" in event for event in events if event.get("event") == "wait"):
        return [f"multiqueue logical waits must not submit GPU work: {counts}"]
    return []


def run(args: argparse.Namespace) -> int:
    if getattr(args, "composition_visual_proof", False) and args.suite != "composition":
        raise ValueError("--composition-visual-proof requires --suite composition")
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
    rr_oracle_dir = getattr(args, "rr_oracle_dir", None)
    rr_oracle_inputs = {}
    rr_native_validation_errors = []
    if args.suite == "ngx-features":
        if rr_oracle_dir is None or rr_oracle_dir.is_symlink() or not rr_oracle_dir.is_dir():
            raise ValueError("--suite ngx-features requires a regular --rr-oracle-dir native oracle directory")
        oracle = out / "rr-oracle"
        oracle.mkdir()
        for name in (*RR_ORACLE_FILES, "owned-rr-native-control", "evidence.json", "stdout.log", "stderr.log", "replay.stdout.log", "replay.stderr.log"):
            source = rr_oracle_dir / name
            if source.is_symlink() or not source.is_file():
                raise ValueError(f"RR oracle input must be a regular non-symlink file: {source}")
            shutil.copy2(source, oracle / name)
            rr_oracle_inputs[name] = digest(oracle / name)
        native_evidence = json.loads((oracle / "evidence.json").read_text())
        if not re.fullmatch(r"OWNED_RR_NATIVE_PASS frames=8 matrix_sensitive_frames=[1-9]\d* source=public-metalfx-denoiser", native_evidence.get("observed_result", "")) or native_evidence.get("finite_non_source_copy_frames") != 32:
            raise ValueError("RR native history oracle execution evidence incomplete")
        if (oracle / "replay.stdout.log").read_text().splitlines().count("OWNED_RR_NATIVE_REPLAY_PASS frames=18 history_frames=8 replay_frame=8 replay_reset=0 source=public-metalfx-denoiser") != 1:
            raise ValueError("RR native history-preserving scene8 replay oracle execution evidence incomplete")
        # A faulty native baseline still permits diagnostic input/RGB comparison, never PASS.
        for name in ("stderr.log", "replay.stderr.log"):
            rr_native_validation_errors.extend(f"RR native {name}: {line}" for line in (oracle / name).read_text().splitlines()
                if any(term in line for term in ("Validation Error", "Invalid Metal usage:", "failed assertion", "OWNED_RR_NATIVE_FAIL")))
        if native_evidence.get("shader_validation_failed") is True and not rr_native_validation_errors:
            rr_native_validation_errors.append("RR native evidence reports shader_validation_failed")
    elif rr_oracle_dir is not None:
        raise ValueError("--rr-oracle-dir is only supported with --suite ngx-features")
    fg_observer = getattr(args, "fg_observer", None)
    if args.suite in ("ngx-alpha", "ngx-fg") and args.mode != "no-present":
        raise ValueError(f"--suite {args.suite} requires --mode no-present")
    if args.suite == "ngx-fg-present" and args.mode != "present":
        raise ValueError("--suite ngx-fg-present requires --mode present")
    if args.suite in ("fsr-fg", "ngx-fg-present") and args.mode == "present" and fg_observer is None:
        raise ValueError(f"--suite {args.suite} --mode present requires --fg-observer for native presentation evidence")
    fg_observer_dependencies = []
    fg_observer_architectures = []
    fg_observer_dependencies_by_architecture = {}
    if fg_observer is not None:
        if args.suite not in ("fsr-fg", "ngx-fg-present"):
            raise ValueError("--fg-observer is only supported with --suite fsr-fg or ngx-fg-present")
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
        sources.append(REPOSITORY_ROOT / "graphics-translator/tests/owned-fg-drawable-observer.mm")
    for source in sources:
        if source.is_symlink() or not source.is_file():
            raise ValueError(f"fixture input must be a regular file: {source}")
        shutil.copy2(source, frozen / source.name)
    executable = frozen / fixture.name
    manifest = json.loads((runtime / "owned-graphics-runtime.json").read_text())
    if args.suite in ("fsr", "fsr-debug", "fsr-fg"):
        selection = "fsr" if args.suite != "fsr-fg" else "fsr_framegeneration"
        if manifest.get("optional_modules", {}).get(selection) is not True:
            raise ValueError(f"--suite {args.suite} requires a runtime staged with --with-fsr ({selection}=true)")
    arch = execution_architecture(manifest,
                                  executable, getattr(args, "guest_arch", None))
    agility_inputs = prepare_agility_inputs(runtime, manifest, arch, frozen) if args.suite == "agility" else None
    env = dict(os.environ)
    for key in tuple(env):
        if (key.startswith(("D3DM", "GPTK_", "DXVK_", "DXMT_", "OWNED_GRAPHICS_", "DYLD_"))
                or key in {"WINEDLLPATH", "WINEDLLPATH_PREPEND", "WINEDLLOVERRIDES",
                           "OWNED_FG_OBSERVER", "FG_CAPTURE_DIR", "FG_CAPTURE_CONSUMER", "FG_CAPTURE_SUITE",
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
    env.pop("OWNED_SHADER_CACHE_CONTROL_ISOLATED", None)
    if fg_observer is not None:
        capture_dir = out / "fg-capture"
        capture_dir.mkdir()
        env["OWNED_FG_OBSERVER"] = str(frozen / fg_observer.name)
        env["FG_CAPTURE_DIR"] = str(capture_dir)
        env["FG_CAPTURE_CONSUMER"] = executable.name
        env["FG_CAPTURE_SUITE"] = args.suite
    if args.suite in ("shader-cache", "pipeline-cache"):
        env["OWNED_SHADER_CACHE_CONTROL_ISOLATED"] = "1"
    if args.suite == "pipeline-cache":
        for phase in ("disk", "corrupt", "truncated", "identity", "native", "failure", "control"):
            (out / f"pipeline-cache-{phase}").mkdir()
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
    if args.suite == "composition":
        command.append("--composition")
        if getattr(args, "composition_visual_proof", False):
            command.extend(("--visual-proof", "--control", "Z:" + str(out / "composition-control.txt").replace("/", "\\")))
    elif args.suite == "ngx-fg-present":
        command.extend(("--mode", "present"))
    elif args.suite == "ngx-alpha":
        command.append("--alpha-only")
    elif args.suite == "ngx-fg":
        command.append("--fg-only")
    elif args.suite == "dxgi-fullscreen":
        command.append("--d3d12-only")
    elif args.suite == "queue-priority":
        command.append("--gpu")
    elif args.suite == "atomic64-pixel-contention":
        command.append("--contention-only")
    elif args.suite == "atomic64-mesh-groupshared":
        command.append("--groupshared64-only")
    elif args.suite.startswith("relaxed-casting-"):
        command.append({"relaxed-casting-shared": "--shared",
                        "relaxed-casting-volume": "--volume",
                        "relaxed-casting-volume-standard": "--volume-standard"}[args.suite])
    elif args.suite == "smoke":
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
    elif args.suite == "dxil-resource-review":
        command.extend(DXIL_RESOURCE_REVIEW_SHADERS)
    elif args.suite in ("arena-heap-arena", "arena-heap-mapping"):
        command.append("arena" if args.suite == "arena-heap-arena" else "mapping")
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
    elif args.suite == "waves":
        command.append("--waves-only")
    elif args.suite == "fp64":
        command.append("--fp64-only")
    elif args.suite == "atomic64-groupshared-capacity":
        command.append("--capacity")
    elif args.suite == "raytracing":
        # The fixture reads exactly one DXR library by name from its own directory, so the
        # frozen file beside the executable is the shader under test.
        command.extend(RAYTRACING_SHADERS)
    elif args.suite == "raytracing-omm":
        command.extend(RAYTRACING_OMM_SHADERS)
    elif args.suite == "raytracing-atomic64":
        command.extend(("--atomic64", "raytracing-contract.dxil", "raytracing-atomic64.dxil"))
    evidence = {
        "schema": 1, "suite": args.suite, "mode": args.mode, "validation": args.validation,
        "validation_mode": validation_mode,
        "architecture": arch,
        "command": command, "prefix": env["WINEPREFIX"],
        "runtime_manifest_sha256": digest(runtime / "owned-graphics-runtime.json"),
        "lock": str(lock), "lock_sha256": digest(lock),
        "fixture_sha256": {source.name: digest(frozen / source.name) for source in sources},
        "rr_oracle_inputs_sha256": rr_oracle_inputs,
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
    core_load_evidence = owned_core_load_evidence(stderr, runtime, manifest, arch, frozen, agility_inputs, require_load=args.suite in ("agility", "diagnostics"))
    foreign.extend(core_load_evidence["foreign_loads"])
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
    if args.suite == "dxgi-factory":
        # This fixture checks actual adapter identities; it has no product export marker.
        marker_ok = native_compiler_proof and all(any(
            f"{module}.dll" in line.lower() and "builtin" in line.lower()
            for line in stderr.splitlines() if "Loaded" in line) for module in ("dxgi", "d3d12"))
    frames = re.findall(r"OWNED_D3D12_SMOKE_FRAME frame=(\d+) readback=ok", stdout)
    barrier_ok = "OWNED_D3D12_SMOKE_BARRIER_STRESS readback=ok" in stdout
    success = (code == 0 and not timed_out and marker_ok and f"{prefix}_PASS" in stdout
               and native_loads and not foreign)
    if args.suite == "dxgi-factory":
        success = (code == 0 and not timed_out and marker_ok and native_loads and not foreign
                   and stdout.splitlines().count("DXGI_FACTORY_EVENTS_OK identity preference nodes occlusion stereo lifetime") == 1)
    corewindow_evidence = None
    composition_public = None
    composition_module = None
    if args.suite == "diagnostics":
        success = success and native_compiler_proof and not diagnostics_failures(stdout, stderr)
    if args.suite == "agility":
        success = success and native_compiler_proof
    if args.suite == "corewindow-composition":
        corewindow_evidence = corewindow_composition_evidence(stdout, stderr)
        success = (code == 0 and not timed_out and marker_ok and native_compiler_proof
                   and native_loads and not foreign and not corewindow_evidence["failures"])
    if args.suite == "composition":
        composition_public = composition_evidence(stdout, stderr, visual=getattr(args, "composition_visual_proof", False))
        composition_module = composition_module_evidence(stderr, runtime, manifest, arch)
        success = (success and native_compiler_proof and validation_mode != "none"
                   and not composition_public["failures"] and not composition_module["failures"])
    fsr_module_loads = []
    if args.suite in ("fsr", "fsr-debug", "fsr-fg"):
        module_name = ("amd_fidelityfx_upscaler_dx12.dll" if args.suite != "fsr-fg"
                       else "amd_fidelityfx_framegeneration_dx12.dll")
        module = rf'Loaded L"C:\\windows\\system32\\{module_name}"'
        fsr_module_loads = [line for line in stderr.splitlines()
                            if module.lower() in line.lower() and "builtin" in line.lower()]
        success = (success and native_compiler_proof and fsr_module_loads
                   and (runtime / arch["windows_directory"] / module_name).is_file())
    dxil_compiler_loaded = False
    if args.suite == "queue-priority":
        success = (success and native_compiler_proof
                   and stdout.splitlines().count(
                       "OWNED_QUEUE_PRIORITY_PASS setters=ignored getters=creation concurrency=4 native_copy=1") == 1
                   and "OWNED_QUEUE_PRIORITY_FAIL" not in stdout + stderr)
    if args.suite == "nvapi-reflex":
        module = "nvapi.dll" if arch["guest_arch"] == "x86" else "nvapi64.dll"
        reports = re.findall(r"^OWNED_NVAPI_REFLEX_PASS cases=8 frames=90 reports=64 real_sleep_us=(\d+)$",
                             stdout, re.MULTILINE)
        success = (success and native_compiler_proof and len(reports) == 1
                   and 25000 <= int(reports[0]) < 1000000
                   and any(module in line.lower() and "builtin" in line.lower()
                           for line in stderr.splitlines() if "Loaded" in line)
                   and "OWNED_NVAPI_REFLEX_FAIL" not in stdout + stderr)
    raytracing_evidence = None
    fence_rewind_cases: list[str] = []
    validation_errors: list[str] = list(rr_native_validation_errors)
    validation_errors.extend(core_load_evidence["failures"])
    if agility_inputs is not None:
        validation_errors.extend(agility_failures(stdout, stderr, frozen / "sdk-owned/d3d12core.dll", agility_inputs["generation"], runtime / agility_inputs["source"]))
    if corewindow_evidence is not None:
        validation_errors.extend(corewindow_evidence["failures"])
    if composition_public is not None:
        validation_errors.extend(composition_public["failures"])
        validation_errors.extend(composition_module["failures"])
        if validation_mode == "none":
            validation_errors.append("Composition requires Metal API or API-and-shader validation")
    if args.suite in ("sr-alpha", "fsr-debug", "ngx-features", "ngx-alpha", "ngx-fg", "dxgi-factory", "dxgi-fullscreen"):
        validation_errors.extend(game_suite_failures(args.suite, stdout, stderr))
        success = success and native_compiler_proof
        dxil_compiler_loaded = native_compiler_proof
    if args.suite in ("ngx-features", "ngx-fg-present", "ngx-alpha", "ngx-fg"):
        success = success and (runtime / arch["windows_directory"] / "nvngx.dll").is_file() and any(
            "nvngx.dll" in line.lower() and "builtin" in line.lower()
            for line in stderr.splitlines() if "Loaded" in line)
        if args.suite not in ("ngx-fg", "ngx-fg-present"):
            success = success and native_compiler_proof
    if args.suite == "ngx-fg-present":
        ngx_present_summary = [dict(re.findall(r"(\w+)=([^\s]+)", line)) for line in stdout.splitlines()
                               if line.startswith("OWNED_D3D12_NGX_PRESENT_PASS ")]
        success = success and len(ngx_present_summary) == 1 and all(
            ngx_present_summary[0].get(key) == value for key, value in
            {"frames": "6", "drawables": "9", "generated": "3", "bootstrap_not_presented": "3",
             "ngx_feature": "11", "hudless": "1", "release_before_execute": "1", "replay": "native_encode",
             "caller_owned_present": "1"}.items())
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
                   and all(int(value) > 0 for value in calibration[0])
                   and not query_ordering_failures(stdout))
    if args.suite == "arena-heap-arena":
        success = (success and stdout.count("OWNED_D3D12_ARENA_HEAP_ARENA_PASS single=20000 shared=24000 "
                                           "indirect=20000 reexecute=2 reset=1 first_error=E_INVALIDARG") == 1
                   and "OWNED_D3D12_ARENA_HEAP_FAIL" not in stdout + stderr)
    if args.suite == "arena-heap-mapping":
        views = re.findall(r"^OWNED_D3D12_ARENA_HEAP_MAPPING_PASS failures=32 views_before=(\d+) views_after=(\d+)$",
                           stdout, re.MULTILINE)
        success = (success and len(views) == 1 and int(views[0][0]) > 0 and views[0][0] == views[0][1]
                   and "OWNED_D3D12_ARENA_HEAP_FAIL" not in stdout + stderr)
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
                   and "OWNED_D3D12_TILED_CAPS tier=4 srv_only_tier3=0 " in stdout
                   and f"OWNED_D3D12_TILED_PASS cases={len(TILED_CASES)} tier=4" in stdout
                   and "OWNED_D3D12_TILED_FAIL" not in stdout + stderr)
    if args.suite == "bindless":
        # Every case line must pass, in order, with the exact caps the fixture checks.
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_BINDLESS_CASE ([\w.-]+) status=PASS$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == BINDLESS_CASES
                   and stdout.count("OWNED_D3D12_BINDLESS_UINT_BORDER variants=24 replays=2 signed=1 "
                                    "narrow_8_16=1 raw_gather_16_32_64=1 empty_view=1 copy_lifetime=1") == 1
                   and "OWNED_D3D12_BINDLESS_CAPS binding_tier=3 heap_tier=2 root_signature=0x3" in stdout
                   and "OWNED_D3D12_BINDLESS_PASS cases=9 heap=1000000 samplers=2048 max_index=999999" in stdout
                   and re.search(r"^OWNED_D3D12_BINDLESS_ROV triangles=512 pixels=256 expected=0x[0-9a-f]{8} "
                                 r"structured_mismatches=0 typed_mismatches=0 texture_mismatches=0$", stdout, re.MULTILINE)
                   and "OWNED_D3D12_BINDLESS_MISMATCH" not in stdout
                   and "OWNED_D3D12_BINDLESS_FAIL" not in stdout + stderr)
    if args.suite == "writable-msaa":
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_SM6_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
        shapes = re.findall(r"^OWNED_D3D12_WRITABLE_MSAA_SHAPE samples=(2|4|8) array=([01])$", stdout, re.MULTILINE)
        scalars = re.findall(r"^OWNED_D3D12_WRITABLE_MSAA_SCALAR type=(sint|float)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == WRITABLE_MSAA_CASES
                   and shapes == [(str(samples), str(array)) for samples in (2, 4, 8) for array in (0, 1)]
                   and scalars == ["sint", "float"]
                   and stdout.count("OWNED_D3D12_WRITABLE_MSAA_CAPS writable=1\n") == 1
                   and stdout.count("OWNED_D3D12_WRITABLE_MSAA_PASS") == 1
                   and "OWNED_D3D12_SM6_MISMATCH" not in stdout
                   and "OWNED_D3D12_SM6_CASE_FAIL" not in stdout
                   and "OWNED_D3D12_SM6_FAIL" not in stdout + stderr
                   and "kIOGPUCommandBufferCallbackError" not in stderr)
    if args.suite == "vectors":
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_SM6_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == VECTOR_CASES
                   and stdout.count("OWNED_D3D12_VECTORS_PASS") == 1
                   and "OWNED_D3D12_SM6_MISMATCH" not in stdout
                   and "OWNED_D3D12_SM6_CASE_FAIL" not in stdout
                   and "OWNED_D3D12_SM6_FAIL" not in stdout + stderr
                   and "kIOGPUCommandBufferCallbackError" not in stderr)
    if args.suite in ("matrix-capabilities", "wave-mma-capabilities"):
        pointer, query_size = ("8", "56") if arch["guest_arch"] == "x64" else ("4", "48")
        if args.suite == "wave-mma-capabilities":
            expected = f"MATRIX_CAPABILITIES_PASS sdk=retired pointer={pointer} query=32 wave_mma_tier=10 shapes=12"
            readback = True
        else:
            expected = f"MATRIX_CAPABILITIES_PASS sdk=successor pointer={pointer} query={query_size} wave_mma_tier=10 linear_algebra_tier=16 operations=6 readback=exact"
            readback = stdout.splitlines().count("MATRIX_RUNTIME_ALLOCATION_LIFETIME_PASS released_resources=1 released_recording=1") == 1
        success = (success and stdout.splitlines().count(expected) == 1 and readback
                   and stdout.splitlines().count("MATRIX_CAPABILITIES_BACKEND id=owned-graphics-d3d12") == 1
                   and "MATRIX_CAPABILITIES_FAIL" not in stdout + stderr
                   and "MATRIX_RUNTIME_FAIL" not in stdout + stderr
                   and "MATRIX_RUNTIME_MISMATCH" not in stdout + stderr
                   and "kIOGPUCommandBufferCallbackError" not in stderr)
    if args.suite == "matrix-runtime":
        conversions = re.findall(r"^MATRIX_RUNTIME_CONVERSIONS_PASS src=(\d+) layouts=16 replay=2 padding=preserved bits=exact$", stdout, re.MULTILINE)
        abi = re.findall(r"^MATRIX_RUNTIME_ABI_PASS pointer=(4|8) union=(48|56) tier=(0|16) legacy=(0|1) pinned_sdk=1$", stdout, re.MULTILINE)
        expected_abi = ("8", "56") if arch["guest_arch"] == "x64" else ("4", "48")
        success = (success and conversions == [str(t) for t in (2,3,4,5,7,8,18,19,20,21)]
                   and len(abi) == 1 and abi[0][:2] == expected_abi
                   and all(stdout.splitlines().count(marker) == 1 for marker in (
                       "MATRIX_RUNTIME_EXPERIMENTAL_PASS unknown=1 malformed=3 unchanged=1 removal=2 lifetime=1",
                       "MATRIX_RUNTIME_ALIGNMENT_PASS sint8_offset=32 float16_offset=64 guards=preserved",
                       "MATRIX_RUNTIME_VALIDATION_PASS invalid_recordings=10 overflow=1",
                       "MATRIX_RUNTIME_ALLOCATION_LIFETIME_PASS released_resources=1 released_recording=1",
                       "MATRIX_RUNTIME_PREDICATION_PASS suppressed=1 executed=1 padding=preserved",
                       "MATRIX_RUNTIME_FENCE_BARRIERS_PASS crossqueue=1 rewind=1 async=1 lifetime=1 state_handoff=1",
                       "MATRIX_RUNTIME_PASS"))
                   and "MATRIX_RUNTIME_FAIL" not in stdout + stderr
                   and "MATRIX_RUNTIME_MISMATCH" not in stdout + stderr
                   and "kIOGPUCommandBufferCallbackError" not in stderr)
    if args.suite == "matrix-shaders":
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_SM6_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == MATRIX_ENTRIES
                   and re.findall(r"^OWNED_D3D12_MATRICES_PASS failures=0$", stdout, re.MULTILINE) == ["OWNED_D3D12_MATRICES_PASS failures=0"]
                   and stdout.count("MATRIX_PUBLIC_SHADER_ADMISSION_PASS") == 1
                   and stdout.count("MATRIX_CONVERTED_VECTOR_CONSUMER_PASS") == 1
                   and "OWNED_D3D12_SM6_MISMATCH" not in stdout
                   and "OWNED_D3D12_SM6_CASE_FAIL" not in stdout + stderr
                   and "OWNED_D3D12_MATRICES_FAIL" not in stdout + stderr
                   and "kIOGPUCommandBufferCallbackError" not in stderr)
    if args.suite == "waves":
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_SM6_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
        rejects = re.findall(r"^OWNED_D3D12_WAVES_REJECT entry=(\w+) hr=0x80004005$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == WAVE_CASES
                   and rejects == ["cs_wavesize64", "cs_wavesize128", "cs_wavesize_range64only"]
                   and re.search(r"^OWNED_D3D12_SM6_CAPS .* wave=1 lanes=4-32 ", stdout, re.MULTILINE)
                   and stdout.count("OWNED_D3D12_WAVES_PASS") == 1
                   and "OWNED_D3D12_SM6_MISMATCH" not in stdout
                   and "OWNED_D3D12_SM6_CASE_FAIL" not in stdout
                   and "OWNED_D3D12_SM6_FAIL" not in stdout + stderr
                   and "kIOGPUCommandBufferCallbackError" not in stderr)
    if args.suite == "fp64":
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_SM6_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == FP64_CASES
                   and re.search(r"^OWNED_D3D12_SM6_CAPS .* double=1 ", stdout, re.MULTILINE)
                   and stdout.count("OWNED_D3D12_FP64_PASS cases=8") == 1
                   and "OWNED_D3D12_SM6_MISMATCH" not in stdout
                   and "OWNED_D3D12_SM6_CASE_FAIL" not in stdout
                   and "OWNED_D3D12_SM6_FAIL" not in stdout + stderr
                   and "kIOGPUCommandBufferCallbackError" not in stderr)
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
                   and re.search(r"^OWNED_D3D12_SM6_CAPS shader_model=0x65/0x0 double=1 minprec=2 wave=1 "
                                 r"lanes=4-32 total_lanes=\d+ int64=1 barycentrics=1 view_instancing=1 "
                                 r"native16=1 atomic64_typed=1 atomic64_groupshared=1 atomic64_heap=1 "
                                 r"wave_mma=10$", stdout, re.MULTILINE)
                   and f"OWNED_D3D12_SM6_PASS stress_reps={SM6_STRESS_REPS}" in stdout
                   and "OWNED_D3D12_SM6_MISMATCH" not in stdout
                   and "OWNED_D3D12_SM6_CASE_FAIL" not in stdout
                   and "OWNED_D3D12_SM6_FAIL" not in stdout + stderr)
    if args.suite == "mesh-shader":
        # Full mesh coverage stays independent of the focused Atomic64 route.
        # Every case passes in order with exact pixels/counters; the caps line is exactly what was verified.
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_MESH_SHADER_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == MESH_SHADER_CASES
                   and "kIOGPUCommandBufferCallbackError" not in stderr
                   and re.search(r"^OWNED_D3D12_MESH_SHADER_CAPS tier=10 stats=1 full_range_rtai=1 derivatives=1 "
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
    if args.suite == "options13":
        success = success and native_compiler_proof and not options13_failures(stdout, stderr)
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
        success = success and not conservative_raster_failures(stdout, stderr)
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
                   and stdout.splitlines().count("OWNED_D3D12_WORK_GRAPHS_LIMIT_RESET_PASS default_restored=1 records=4") == 1
                   and stdout.splitlines().count("OWNED_D3D12_WORK_GRAPHS_LIMIT_REJECT_PASS records=4 max=3 nodes=1 max_nodes=1") == 1
                   and "OWNED_D3D12_WORK_GRAPHS_PASS cases=9 tier=1_0" in stdout
                   and "OWNED_D3D12_WORK_GRAPHS_FAIL" not in stdout + stderr)
    if args.suite == "workgraphs-atomic64":
        dxil_compiler_loaded = native_compiler_proof
        wg_atomic64_failures = workgraphs_atomic64_failures(stdout, stderr)
        validation_errors.extend(wg_atomic64_failures)
        success = success and dxil_compiler_loaded and not wg_atomic64_failures
    if args.suite == "atomic64-helpers":
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_ATOMIC64_HELPERS_CASE mode=(\d+) samples=(\d+) status=PASS$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded
                   and cases == [(str(mode), str(samples)) for mode, samples in enumerate((1, 4, 1, 1, 4, 1, 1, 8, 8, 8, 4))]
                   and stdout.splitlines().count("OWNED_D3D12_ATOMIC64_HELPERS_PASS cases=11 failures=0") == 1
                   and "OWNED_D3D12_SM6_MISMATCH" not in stdout
                   and "OWNED_D3D12_SM6_CASE_FAIL" not in stdout + stderr
                   and "OWNED_D3D12_SM6_FAIL" not in stdout + stderr
                   and "OWNED_D3D12_ATOMIC64_HELPERS_FAIL" not in stdout + stderr)
    if args.suite == "atomic64-mesh-groupshared":
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_MESH_SHADER_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and cases == ["groupshared64_ms", "groupshared64_as"]
                   and stdout.splitlines().count("OWNED_D3D12_MESH_SHADER_PASS groupshared64=2 failures=0") == 1
                   and not re.search(r"OWNED_D3D12_MESH_SHADER_(?:MISMATCH|CASE_FAIL|FAIL)", stdout + stderr))
    if args.suite == "atomic64-pixel-coupled":
        dxil_compiler_loaded = native_compiler_proof
        pixel_rows = re.findall(r"^OWNED_D3D12_ATOMIC64_PIXEL_CASE rov=([01]) typed=([01]) sample=([01]) mode=([0-4]) status=PASS$", stdout, re.MULTILINE)
        expected_pixel_rows = [(str(rov), str(typed), str(sample), str(mode))
                               for rov in range(2) for typed in range(2) for sample in range(2) for mode in range(5)]
        begins = re.findall(r"^OWNED_D3D12_ATOMIC64_PIXEL_BEGIN rov=([01]) typed=([01]) sample=([01]) mode=([0-4])$", stdout, re.MULTILINE)
        cases = re.findall(r"^OWNED_D3D12_SM6_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and pixel_rows == begins == expected_pixel_rows
                   and cases == ["atomic64_pixel_coupled_color_depth_stencil_query"] * 40
                   and stdout.splitlines().count("OWNED_D3D12_ATOMIC64_PIXEL_PASS cases=40 failures=0") == 1
                   and len(re.findall(r"^OWNED_D3D12_ATOMIC64_PIXEL_CAP typed=1$", stdout, re.MULTILINE)) == 1
                   and not re.search(r"OWNED_D3D12_SM6_(?:MISMATCH|CASE_FAIL|FAIL)|OWNED_D3D12_ATOMIC64_PIXEL_FAIL", stdout + stderr))
    if args.suite == "atomic64-pixel-contention":
        dxil_compiler_loaded = native_compiler_proof
        contention_rows = re.findall(r"^OWNED_D3D12_ATOMIC64_PIXEL_CONTENTION_CASE operation=(add|cas) targets=(1|16) "
                                     r"fragments=4096 old_values=4096 pixels=4096 status=PASS$", stdout, re.MULTILINE)
        begins = re.findall(r"^OWNED_D3D12_ATOMIC64_PIXEL_CONTENTION_BEGIN operation=(add|cas) targets=(1|16) fragments=4096$",
                            stdout, re.MULTILINE)
        expected_contention = [(operation, str(targets)) for operation in ("add", "cas") for targets in (1, 16)]
        cases = re.findall(r"^OWNED_D3D12_SM6_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and contention_rows == begins == expected_contention
                   and stdout.count("OWNED_D3D12_ATOMIC64_PIXEL_CONTENTION_CASE ") == 4
                   and stdout.count("OWNED_D3D12_ATOMIC64_PIXEL_CONTENTION_BEGIN ") == 4
                   and cases == ["atomic64_pixel_contention_full64_multiset_color_guards"] * 4
                   and stdout.splitlines().count("OWNED_D3D12_ATOMIC64_PIXEL_CONTENTION_PASS cases=4 fragments=4096 failures=0") == 1
                   and len(re.findall(r"^OWNED_D3D12_ATOMIC64_PIXEL_CONTENTION_CAP typed=1$", stdout, re.MULTILINE)) == 1
                   and not re.search(r"OWNED_D3D12_SM6_(?:MISMATCH|CASE_FAIL|FAIL)|OWNED_D3D12_ATOMIC64_PIXEL_CONTENTION_FAIL", stdout + stderr))
    if args.suite in ("atomic64-groupshared", "atomic64-groupshared-capacity"):
        dxil_compiler_loaded = native_compiler_proof
        capacity = args.suite.endswith("-capacity")
        mode = "capacity" if capacity else "operations-stress"
        expected = ["atomic64_groupshared_32k_capacity"] if capacity else [
            "atomic64_groupshared_operations", "atomic64_groupshared_stress"]
        cases = re.findall(r"^OWNED_D3D12_SM6_CASE_PASS (\w+)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and cases == expected
                   and len(re.findall(r"^OWNED_D3D12_SM6_CAPS .* atomic64_groupshared=1 ", stdout, re.MULTILINE)) == 1
                   and stdout.splitlines().count(f"OWNED_D3D12_ATOMIC64_RESULT mode={mode} failures=0") == 1
                   and stdout.splitlines().count(f"OWNED_D3D12_ATOMIC64_PASS mode={mode}") == 1
                   and not re.search(r"OWNED_D3D12_SM6_(?:MISMATCH|CASE_FAIL|FAIL)|OWNED_D3D12_ATOMIC64_FAIL",
                                     stdout + stderr))
    if args.suite == "atomic64-producers":
        dxil_compiler_loaded = native_compiler_proof
        producer_failures = atomic64_producers_failures(stdout, stderr)
        validation_errors.extend(producer_failures)
        success = success and dxil_compiler_loaded and not producer_failures
    if args.suite == "atomic64-typed":
        dxil_compiler_loaded = native_compiler_proof
        typed_failures = atomic64_typed_failures(stdout, stderr)
        validation_errors.extend(typed_failures)
        success = success and dxil_compiler_loaded and not typed_failures
    if args.suite == "workgraphs-atomic64-typed":
        dxil_compiler_loaded = native_compiler_proof
        wg_typed_failures = workgraphs_atomic64_typed_failures(stdout, stderr)
        validation_errors.extend(wg_typed_failures)
        success = success and dxil_compiler_loaded and not wg_typed_failures
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
                   and stdout.count("OWNED_D3D12_SHARING_ADAPTER_LUID_PASS") == 1
                   and "OWNED_D3D12_SHARING_CAPS cross_node=0 row_major=1" in stdout
                   and f"OWNED_D3D12_SHARING_PASS cases={len(SHARING_CASES)}" in stdout
                   and "OWNED_D3D12_SHARING_FAIL" not in stdout + stderr
                   and "OWNED_D3D12_SHARING_MISMATCH" not in stdout + stderr)
    if args.suite == "residency":
        success = (success
                   and stdout.count("OWNED_D3D12_RESIDENCY_BUDGET_PASS") == 1
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
    if args.suite == "atomic64-phase":
        dxil_compiler_loaded = native_compiler_proof
        success = success and dxil_compiler_loaded and not atomic64_phase_failures(stdout, stderr)
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
        module_loads = (runtime / arch["windows_directory"] / "d3d11.dll").is_file() and any(
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
        programmable = re.findall(
            r"^OWNED_D3D12_MULTISAMPLE_CAPABILITIES_PROGRAMMABLE_GPU_PASS count=(\d+) depth=(\d+)$",
            stdout, re.MULTILINE)
        resolves = re.findall(
            r"^OWNED_D3D12_MULTISAMPLE_CAPABILITIES_RESOLVE_GPU_PASS count=(\d+) depth=(\d+) "
            r"mode=(\d+) no_target_flag=1$", stdout, re.MULTILINE)
        integer_resolves = re.findall(
            r"^OWNED_D3D12_MULTISAMPLE_CAPABILITIES_RESOLVE_INTEGER_GPU_PASS count=(\d+) stencil=(\d+) "
            r"mode=(\d+) no_target_flag=1$", stdout, re.MULTILINE)
        decompressions = re.findall(
            r"^OWNED_D3D12_MULTISAMPLE_CAPABILITIES_DECOMPRESS_GPU_PASS count=(\d+) depth=(\d+) "
            r"plane=(\d+) partial=1 no_target_flag=1$", stdout, re.MULTILINE)
        success = (success and len(gpu_cases) == len(expected_cases)
                   and set(gpu_cases) == expected_cases
                   and len(programmable) == 6
                   and set(programmable) == {(str(count), str(depth))
                                             for count in (2, 4, 8) for depth in (0, 1)}
                   and len(resolves) == 18
                   and set(resolves) == {(str(count), str(depth), str(mode))
                                         for count in (2, 4, 8) for depth in (0, 1) for mode in (1, 2, 3)}
                   and len(integer_resolves) == 12
                   and set(integer_resolves) == {(str(count), str(stencil), str(mode))
                                                 for count in (2, 4, 8) for stencil in (0, 1) for mode in (1, 2)}
                   and len(decompressions) == 12
                   and set(decompressions) == {(str(count), str(depth), str(plane))
                                               for count in (1, 2, 4, 8) for depth in (0, 1)
                                               for plane in range(depth + 1)}
                   and stdout.count("OWNED_D3D12_MULTISAMPLE_CAPABILITIES_PROGRAMMABLE_LIFECYCLE_PASS "
                                    "switches=2 replay=6 reset=6 clearstate=6 crosslist=6 bundles=6") == 1
                   and stdout.count("OWNED_D3D12_MULTISAMPLE_CAPABILITIES_PROGRAMMABLE_ERRORS_PASS "
                                    "rejected=13 first_error=E_INVALIDARG") == 1
                   and stdout.count("OWNED_D3D12_MULTISAMPLE_CAPABILITIES_RESOLVE_LIFECYCLE_PASS "
                                    "inplace=8 separate=8 partial=12 planes=4") == 1
                   and stdout.count("OWNED_D3D12_MULTISAMPLE_CAPABILITIES_CUSTOM_DEPTH_COPY_GPU_PASS "
                                    "count=4 planes=2 other_plane=1") == 1
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
        number = r"(-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)"
        compatibility = re.findall(
            r"^OWNED_D3D12_NGX_BEHAVIOR_PASS case=(\w+) replay=(\d+) exposure_scale=" + number
            + r" mv_offset_x=" + number + r" mv_offset_y=" + number
            + r" max_byte_delta=(\d+) sentinel=1 live_inputs=1 params_destroyed=1 "
              r"released_before_submit=1 highres_jittered=([01])$", stdout, re.MULTILINE)
        expected_compatibility = {
            "exposure_scale_auto": (2.0, 0.0, 0.0, 0),
            "exposure_scale_texture": (2.0, 0.0, 0.0, 0),
            "exposure_scale_no_texture": (0.375, 0.0, 0.0, 0),
            "mv_value_offsets_lowres": (1.0, -1.25, 2.5, 0),
            "mv_value_offsets_highres_jittered": (1.0, -0.375, 0.625, 1),
        }
        success = (success and frames == [str(n) for n in range(8)]
                   and [(name, int(replay)) for name, replay, *_ in compatibility]
                       == [(name, replay) for name in expected_compatibility for replay in range(3)]
                   and all((float(scale), float(offset_x), float(offset_y), int(highres))
                           == expected_compatibility[name] and int(delta) <= 1
                           for name, _, scale, offset_x, offset_y, delta, highres in compatibility)
                   and stdout.count("OWNED_D3D12_NGX_BEHAVIOR_PASS case=rejected_output_preserved "
                                    "invalid_input_bounds=1 invalid_compatibility_scalars=5 sentinel=1") == 1)
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
        callbacks = re.findall(r"OWNED_D3D12_FSR_FG_CALLBACK_PIXELS frame=(\d+) generated=(\d+) output_index=(\d+) width=128 height=128 "
                               r"hash=([0-9a-f]{16}) marker=(\d+),(\d+),220", stdout)
        generated_counts = dict.fromkeys((1, 2, 3, 4, 7, 8, 10, 11, 12), 1)
        expected_callbacks = []
        for frame in range(1, 13):
            expected_callbacks.extend((frame, 1, index) for index in range(1, generated_counts.get(frame, 0) + 1))
            if frame not in (8, 12):
                expected_callbacks.append((frame, 0, 0))
        grouped = re.findall(r"^OWNED_D3D12_FSR_FG_NATIVE_PASS count=(1) distinct_gpu_outputs=(1) "
                             r"rect_guard=exact reset_output=current$", stdout, re.MULTILINE)
        ui_frames = re.findall(r"OWNED_D3D12_FSR_FG_UI_FRAME frame=(\d+) width=(\d+) height=(\d+) source=(\d+),60,100 "
                               r"ui=64,0,0,128 premul=1 doublebuffer=1 overwritten_after_present=1", stdout)
        statistics = re.findall(r"OWNED_D3D12_FSR_FG_STATISTICS frame=(\d+) statistics=(actual|disjoint) "
                                r"result=(0x[0-9a-fA-F]+) submitted_present=(\d+) native_present=(\d+) "
                                r"present_refresh=(\d+) sync_refresh=(\d+) sync_qpc=(\d+) qpc_end=(\d+) frequency=(\d+) "
                                r"source=public_dxgi_actual_presentation", stdout)
        final_statistics = re.findall(r"OWNED_D3D12_FSR_FG_FINAL_STATISTICS stream=(callback|builtin_ui) "
                                      r"output_vblanks=3 statistics=(actual|disjoint) result=(0x[0-9a-fA-F]+) "
                                      r"submitted_present=(\d+) native_present=(\d+) present_refresh=(\d+) "
                                      r"sync_refresh=(\d+) sync_qpc=(\d+) qpc_end=(\d+) frequency=(\d+) "
                                      r"source=public_dxgi_actual_presentation", stdout)
        observations_ok = len(statistics) == 16 and [row[0] for row in final_statistics] == ["callback", "builtin_ui"]
        if observations_ok:
            for rows, final, requested in zip((statistics[:12], statistics[12:]), final_statistics,
                                              (len(expected_callbacks), len(ui_frames))):
                previous = (0, 0, 0, 0)
                previous_submitted = 0
                frequencies = set()
                for _, outcome, result, submitted, count, refresh, sync, qpc, end, frequency in [*rows, final]:
                    submitted, count, refresh, sync, qpc, end, frequency = map(int, (submitted, count, refresh, sync, qpc, end, frequency))
                    values = (count, refresh, sync, qpc)
                    observations_ok &= submitted >= previous_submitted and frequency > 0 and end > 0
                    frequencies.add(frequency)
                    if outcome == "actual":
                        observations_ok &= (int(result, 0) == 0 and 0 < count <= submitted and 0 < refresh <= sync and
                                            0 < qpc <= end and all(a <= b for a, b in zip(previous, values)))
                        previous = values
                    else:
                        observations_ok &= int(result, 0) == 0x887A000B and values == (0, 0, 0, 0)
                    previous_submitted = submitted
                observations_ok &= (final[1] == "actual" and final[3] == rows[-1][3] and
                                    int(final[3]) == requested and len(frequencies) == 1)
        success = (success
                   and stdout.count("OWNED_D3D12_FSR_FG_ORIGINAL_SDK_PASS resize1=unsupported unchanged=1 resize=implemented pacing=original") == 1
                   and "OWNED_D3D12_FSR_FG_DIRECT_PASS snapshot=exact rect_guard=exact frames=4" in stdout
                   and "OWNED_D3D12_FSR_FG_HISTORY_PASS reset=fresh gap=reset off_on=fresh stale_record=no_history_publication negative_output=unchanged" in stdout
                   and "OWNED_D3D12_FSR_FG_LIFETIME_PASS context_destroy_before_execute=1 input_release=1 allocation_balance=1" in stdout
                   and "OWNED_D3D12_FSR_FG_CONFIG_PASS repeated_id=replace retained_distinct_limit=64 overflow=runtime_error off_clears=1" in stdout
                   and all(stdout.splitlines().count(marker) == 1 for marker in (
                       "OWNED_D3D12_FSR_FG_DEBUG_PASS tear=gpu_effect reset=history debug=real_inputs configure=frame_snapshot history=unmodified alpha=opaque",
                       "OWNED_D3D12_FSR_FG_DEBUG_CAPTURE_PASS prepare=per_frame configure=recorded rect=recorded output=independent_gpu_exact",
                       "OWNED_D3D12_FSR_FG_DEBUG_REPLAY_PASS combined=1 debug_overlay=full_display replay=fresh_native context_destroy_before_replay=1",
                       "OWNED_D3D12_FSR_FG_COUNT_REJECTION_PASS invalid=0,5 unsupported=2,3,4 sentinel=exact prepare=retained",
                       "OWNED_D3D12_FSR_FG_NO_NOTIFY_PROXY_PASS frame=3 generated=1",
                       "OWNED_D3D12_FSR_FG_PREPARE_ORDER_PASS matching_frame=2 newest_prepare=3 output=exact",
                       "OWNED_D3D12_FSR_FG_NATIVE_PACING_PASS count=1 generated_before_real=1 bootstrap_reset=native_output disabled=real_only callback_error=real_only"))
                   and grouped == [("1", "1")]
                   and all(len({row[3] for row in callbacks if int(row[0]) == frame and row[1] == "1"}) == count
                           for frame, count in generated_counts.items())
                   and "OWNED_D3D12_FSR_FG_PROXY_PASS create=hwnd callbacks=gpu-readback generated_before_real=1 only_generated=1 disabled=1 callback_error=1 drain=1 destroy=1" in stdout
                   and "OWNED_D3D12_FSR_FG_PACING_PASS ordered=1 bounded_progress=1 qpc=observed native_timestamps=external_observer_required" in stdout
                   and "OWNED_D3D12_FSR_FG_UI_PASS wrap=real_hwnd ui=builtin_shader premultiplied=1 doublebuffer=gpu_snapshot resize=96x80 lifetime=drained" in stdout
                   and [tuple(map(int, row)) for row in ui_frames]
                       == [(11, 128, 128, 88), (12, 128, 128, 96), (13, 96, 80, 104), (14, 96, 80, 112)]
                   and [int(frame) for frame, *_ in statistics] == [*range(1, 13), *range(11, 15)]
                   and observations_ok
                   and [int(frame) for frame, *_ in present_calls] == list(range(1, 13))
                   and [(int(frame), int(generated), int(index)) for frame, generated, index, *_ in callbacks] == expected_callbacks
                   and all(int(red) == int(frame) + 32 and int(green) == (190 if generated == "1" else 70)
                           for frame, generated, _, _, red, green in callbacks)
                   and all((int(enabled), int(only), int(generation_error), int(present_error))
                           == (int(int(frame) not in (5, 6)), int(frame in ("8", "12")), int(frame == "9"), 0)
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
    if args.suite == "dxil-resource-review":
        dxil_compiler_loaded = native_compiler_proof
        success = (success and dxil_compiler_loaded
                   and not dxil_resource_review_failures(stdout)
                   and stdout.count("OWNED_D3D12_DXIL_RESOURCE_REVIEW_PASS") == 1
                   and "OWNED_D3D12_DXIL_RESOURCE_REVIEW_FAIL" not in stdout + stderr)
    if args.suite == "advanced-texture":
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_ADVANCED_TEXTURE_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == ADVANCED_TEXTURE_CASES
                   and len(re.findall(r"^OWNED_D3D12_ADVANCED_TEXTURE_CAPS relaxed_format_casting=1 advanced_texture_ops=1$", stdout, re.MULTILINE)) == 1
                   and stdout.count("OWNED_D3D12_ADVANCED_TEXTURE_PASS cases=24") == 1
                   and "OWNED_D3D12_ADVANCED_TEXTURE_FAIL" not in stdout + stderr
                   and "OWNED_D3D12_DXIL_FAIL" not in stdout + stderr
                   and "kIOGPUCommandBufferCallbackError" not in stderr)
    if args.suite == "depth-casting":
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_DEPTH_CASTING_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == DEPTH_CASTING_CASES
                   and stdout.count("OWNED_D3D12_DEPTH_CASTING_PASS cases=8") == 1
                   and "OWNED_D3D12_DEPTH_CASTING_FAIL" not in stdout + stderr
                   and "OWNED_D3D12_DXIL_FAIL" not in stdout + stderr
                   and "kIOGPUCommandBufferCallbackError" not in stderr)
    if args.suite == "depth-bounds":
        dxil_compiler_loaded = native_compiler_proof
        depth_bounds_errors = depth_bounds_failures(stdout, stderr)
        validation_errors.extend(depth_bounds_errors)
        success = success and dxil_compiler_loaded and not depth_bounds_errors
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
                   and stdout.count("OWNED_D3D12_DXIL_TEXTURE_OFFSETS_PASS batches=2 lanes=16 "
                                    "families=22 components=2816") == 1
                   and "OWNED_D3D12_DXIL_TEXTURE_OFFSETS_FAIL" not in stdout + stderr
                   and stdout.count("OWNED_D3D12_DXIL_TEXTURE_PASS") == 1)
    if args.suite == "raytracing-omm":
        dxil_compiler_loaded = native_compiler_proof
        subdivisions = re.findall(r"^OMM_SUBDIVISION format=(\d+) level=(\d+) samples=(\d+) cumulative=(\d+) failures=0$",
                                  stdout, re.MULTILINE)
        expected = [(str(fmt), str(level), str(min(4**level, 32) if level > 5 else 4**level))
                    for fmt in (1, 2) for level in range(13)] + [("2", "1", "4")]
        results = re.findall(r"^OWNED_D3D12_OMM_RESULT comparisons=(\d+) failures=0 legacy=([01])$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and [row[:3] for row in subdivisions] == expected
                   and results == [("56206", "0"), ("3604", "1")]
                   and stdout.splitlines().count("OWNED_D3D12_OMM_PASS cases=27") == 1
                   and stdout.splitlines().count("OMM_SERIALIZED_BLAS_HEADER postamble=4294967295 blocks=1 blocktype=0 pointers=1") == 2
                   and stdout.splitlines().count("OWNED_D3D12_OMM_CAPS tier=12 actually_reorders=0 byte_offset_views=0 max_dispatch=65535 max_mesh=65535 query_abi=16 errors=8") == 1
                   and stdout.splitlines().count("OMM_DRIVER_IDENTIFIER_PASS type=0 array=1 blas=1 rejects=3") == 2
                   and not re.search(r"OMM_[A-Z_]*FAIL|OWNED_D3D12_RAYTRACING_FAIL", stdout + stderr))
    if args.suite == "raytracing-atomic64":
        dxil_compiler_loaded = native_compiler_proof
        atomic_rt_failures = raytracing_atomic64_failures(stdout, stderr)
        raytracing_evidence = {"lanes": re.findall(r"^OWNED_D3D12_RAYTRACING_ATOMIC64_LANE lane=(\d+) ",
                                                 stdout, re.MULTILINE), "failures": atomic_rt_failures}
        success = success and dxil_compiler_loaded and not atomic_rt_failures
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
                   and contract_cases == RAYTRACING_CONTRACT_CASES + RAYTRACING_STATEOBJ_CASES + ("hitobject", "hitobject-reorder-coherence")
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
    log_paths = sorted(out.glob("*_d3d12.log"))
    if args.suite == "pipeline-cache":
        log_paths += sorted(out.glob("pipeline-cache-*/*_d3d12.log"))
    backend_logs = {str(path.relative_to(out)): path.read_text(errors="replace") for path in log_paths}
    diagnostic_text = "\n".join([stderr, *backend_logs.values()])
    validation_errors += [line for line in diagnostic_text.splitlines()
                         if any(term in line for term in ("validateCompute", "validateRender",
                                                           "Execution of the command buffer was aborted",
                                                           "Shader Validation Error", "Invalid Metal usage:",
                                                           "Device error:", "failed assertion `",
                                                           "Metal4 bridge operation failed"))]
    if args.suite == "options13":
        validation_errors.extend(options13_failures(stdout, stderr))
    rr_analysis = None
    if args.suite == "ngx-features":
        rr_analysis = rr_oracle_comparison(frozen, out / "rr-oracle")
        validation_errors.extend(rr_analysis["failures"])
    if args.suite.startswith("relaxed-casting"):
        validation_errors.extend(relaxed_casting_failures(args.suite, stdout, stderr))
        success = success and native_compiler_proof
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
    if args.suite == "multiqueue":
        validation_errors.extend(multiqueue_submission_failures(trace_events))
    if args.suite == "pipeline-cache":
        validation_errors.extend(pipeline_cache_failures(stdout, stderr, backend_logs))
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
    if args.suite in ("fsr-fg", "ngx-fg-present"):
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
                 "--suite", args.suite,
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
              "owned_core_module_loads": core_load_evidence,
              "owned_fsr_module_loads": fsr_module_loads,
              "rr_oracle_comparison": rr_analysis,
              "fg_output_artifacts": fg_output_artifacts, "fg_capture_analysis": fg_capture_analysis,
              "dxil_native_compiler_verified": dxil_compiler_loaded,
              "atomic64_pixel": ({"ordered_case_rows": pixel_rows,
                                  "required_oracles": ["full64_old_final", "ordinary32_alias", "coupled_color",
                                                       "depth_stencil_positive_negative", "occlusion_pipeline_statistics"]}
                                 if args.suite == "atomic64-pixel-coupled" else None),
              "atomic64_pixel_contention": ({"ordered_case_rows": contention_rows, "fragments_per_case": 4096,
                                             "required_oracles": ["full64_returned_old_multisets", "full64_final",
                                                                  "prefix_suffix_unused_guards", "all_4096_pixels"],
                                             "native_bitmap_hang_proof_replaced": False}
                                            if args.suite == "atomic64-pixel-contention" else None),
              "atomic64_typed": ({"capability_rows": re.findall(r"^OWNED_D3D12_ATOMIC64_TYPED_CAP typed=([01])$", stdout, re.MULTILINE),
                                  "shape_sign_rows": re.findall(r"^OWNED_D3D12_ATOMIC64_TYPED_CASE shape=(\d+) signed=(\d+)$", stdout, re.MULTILINE),
                                  "format_rows": re.findall(r"^OWNED_D3D12_ATOMIC64_TYPED_FORMAT(_UNSUPPORTED)? format=(\d+) (?:unit|support2)=(\d+)$", stdout, re.MULTILINE),
                                  "failures": typed_failures}
                                 if args.suite == "atomic64-typed" else None),
              "workgraphs_atomic64": ({"library_profile": "lib_6_8",
                                       "expected_readback_qwords": 384,
                                       "record_rows": re.findall(r"^OWNED_D3D12_ATOMIC64_WORKGRAPH_RECORD mode=(CPU|GPU) index=(\d+) ", stdout, re.MULTILINE),
                                       "failures": wg_atomic64_failures}
                                      if args.suite == "workgraphs-atomic64" else None),
              "workgraphs_atomic64_typed": ({"expected_readback_qwords": 48,
                                             "texel_indices": re.findall(r"^OWNED_D3D12_ATOMIC64_WG_TYPED_TEXEL index=(\d+) ", stdout, re.MULTILINE),
                                             "failures": wg_typed_failures}
                                            if args.suite == "workgraphs-atomic64-typed" else None),
              "raytracing": raytracing_evidence,
              "fence_rewind_cases": fence_rewind_cases,
              "foreign_backend_loads": foreign, "validation_errors": validation_errors,
              "rr_native_validation_errors": rr_native_validation_errors,
              "backend_logs": list(backend_logs), "trace_event_count": len(trace_events),
              "temporal_analysis": temporal_analysis,
              "pass": success and args.suite != "corewindow-composition",
              "scope": ("reachable-only genuine CoreWindow contracts; separate runtime-object prerequisite; not feature complete"
                        if args.suite == "corewindow-composition" else
                        "public Composition GPU/contracts; on-screen alpha/matrix requires independent capture; not CoreWindow completion"
                        if args.suite == "composition" else
                        "synthetic owned D3D12/D3D11 fullscreen fixture; not general game compatibility"
                        if args.suite == "fullscreen" else
                        "synthetic owned D3D12 fixture; not ZZZ compatibility or a nameplate fix")}
    if agility_inputs is not None:
        report["agility_sdk"] = agility_inputs
    if corewindow_evidence is not None:
        report.update({"result": "REACHABLE_OK" if success else "FAIL",
                       "reachable_only_verified": bool(success), "feature_complete": False,
                       "corewindow_composition": corewindow_evidence})
    if composition_public is not None:
        report.update({"result": "PASS" if success else "FAIL", "feature_complete": False,
                       "composition": composition_public, "composition_module": composition_module})
    (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(stdout, end="")
    verdict = ("REACHABLE_OK" if args.suite == "corewindow-composition" else "PASS") if success else "FAIL"
    print(f"{verdict} owned graphics {args.mode} "
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
    parser.add_argument("--rr-oracle-dir", type=Path,
                        help="independently executed native RR history/reset artifacts; required for ngx-features and frozen before launch")
    parser.add_argument("--fg-observer", type=Path,
                        help="x86_64 or universal x86_64/arm64 native drawable observer dylib, required for fsr-fg/ngx-fg-present --mode present; frozen as evidence")
    parser.add_argument("--dxil-dir", type=Path, default=DEFAULT_DXIL_DIR,
                        help="directory holding the frozen .dxil inputs a suite reads by name "
                             "(compute/vertex/pixel/register-space for the DXIL suites, "
                             "raytracing.dxil for ray tracing, atomic-copy-reader.dxil for "
                             "atomic-copy and atomic-copy-live)")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--composition-visual-proof", action="store_true",
                        help="hold Composition windows for independent capture; advance by closing an owned window or writing the exact phase to OUT/composition-control.txt")
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
