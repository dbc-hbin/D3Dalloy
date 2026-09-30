#!/usr/bin/env python3
"""Run a source-built owned graphics fixture in a fresh, isolated Wine prefix."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
from typing import NamedTuple


SUITE_PREFIXES = {
    "smoke": "OWNED_D3D12_SMOKE",
    "multiqueue": "OWNED_D3D12_MULTIQUEUE",
    "capabilities": "OWNED_CAPABILITIES",
    "temporal": "OWNED_D3D12_TEMPORAL",
    "ngx": "OWNED_D3D12_NGX",
    "ngx-contracts": "OWNED_D3D12_NGX_CONTRACTS",
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
}
REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SHARING_CASES = (
    "buffer-a_to_b", "texture-a_to_b", "fence-a_to_b",
    "buffer-b_to_a", "texture-b_to_a", "fence-b_to_a",
    "cpu-signal-gpu-wait", "heap-placed", "heap-row-major-texture", "named-open", "errors",
    "fence-rewind", "fence-stress", "named-unrelated-process", "host-memory-address", "host-memory-file-mapping",
    "cross-process-parent-to-child", "cross-process-child-to-parent",
)
DEFAULT_LOCK = REPOSITORY_ROOT / "graphics-translator/upstream.lock.json"
DEFAULT_DXIL_DIR = REPOSITORY_ROOT / "graphics-translator/tests/dxil"
# NGX fixtures run on the temporal backend and publish its identity line.
BACKEND_PREFIXES = {
    "ngx": "OWNED_D3D12_TEMPORAL",
    "ngx-contracts": "OWNED_D3D12_TEMPORAL",
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
PARTIAL_3D_UAV_SHADERS = ("partial-3d-uav.dxil",)
# SM: frozen DXIL read by name (sm6-<entry>.dxil) beside the executable; the HLSL and
# its build script are frozen with them so the evidence names the exact sources.
SM6_SOURCES = ("sm6.hlsl", "sm6-16bit.hlsl", "sm6-heap.hlsl", "sm6-reject.hlsl", "sm6-denorm.hlsl", "build-sm6.sh")
SM6_ENTRIES = (
    "cs_wave", "cs_divergent", "cs_int64", "cs_atomic64", "cs_atomic64_ops", "cs_atomic64_heap", "cs_minprec",
    "cs_quad_deriv", "vs_main", "ps_main", "ps_helper", "ps_helper_all", "vs_tiny", "ps_atomic64", "ps_umax64",
    "vs_int64", "ps_int64", "vs_wave", "ps_null", "cs_16bit", "cs_packed", "vs_16bit", "ps_16bit", "cs_wavesize32", "cs_wavesize64",
    "cs_fp64", "cs_gs64", "cs_sm69", "vs_viewid", "ps_viewid", "vs_shadingrate", "ps_shadingrate", "cs_rayquery",
    "ps_feedback", "ps_viewid_vi", "ms_mesh", "cs_denorm_any", "cs_denorm_ftz", "cs_denorm_preserve",
)
SM6_CASES = (
    "wave", "divergent", "int64", "atomic64_ops", "atomic64_heap", "16bit", "minprec", "packed", "quad_deriv",
    "rejections", "wavesize32", "barycentrics", "barycentrics_indexed", "helper_lanes", "helper_lanes_included",
    "16bit_io", "graphics_int64", "vs_wave", "graphics_atomic64_rejected", "sv_viewid",
    "cap_rejected", "denorm_any", "denorm_ftz", "denorm_preserve", "cs_atomic64_stress_1", "cs_atomic64_stress_16",
)
SM6_REJECTS = ["cs_wavesize64", "cs_fp64", "cs_gs64", "cs_sm69", "ps_atomic64", "ps_umax64", "ps_shadingrate",
                "cs_rayquery", "ps_feedback", "ps_viewid_vi", "ms_mesh"]
SM6_STRESS_REPS = 20
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
RAYTRACING_SHADERS = ("raytracing.dxil",)
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
        return [dxil_dir / name for name in STREAM_OUTPUT_SHADERS]
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
        return [dxil_dir / name for name in names] + sorted(dxil_dir.glob(f"{stem}-*.dxil"))
    if suite == "bindless":
        return [dxil_dir / name for name in BINDLESS_SHADERS + BINDLESS_INPUTS]
    if suite == "pipeline-statistics":
        return [dxil_dir / name for name in PIPELINE_STATISTICS_SHADERS + PIPELINE_STATISTICS_STAGE_INPUTS]
    if suite == "sm6":
        return [dxil_dir / f"sm6-{entry}.dxil" for entry in SM6_ENTRIES] + [dxil_dir / name for name in SM6_SOURCES]
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
    for source in sources:
        if source.is_symlink() or not source.is_file():
            raise ValueError(f"fixture input must be a regular file: {source}")
        shutil.copy2(source, frozen / source.name)
    executable = frozen / fixture.name
    env = dict(os.environ)
    for key in tuple(env):
        if (key.startswith(("D3DM", "GPTK_", "DXVK_", "DXMT_", "OWNED_GRAPHICS_", "DYLD_"))
                or key in {"WINEDLLPATH", "WINEDLLPATH_PREPEND", "WINEDLLOVERRIDES",
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
        "command": command, "prefix": env["WINEPREFIX"],
        "runtime_manifest_sha256": digest(runtime / "owned-graphics-runtime.json"),
        "lock": str(lock), "lock_sha256": digest(lock),
        "fixture_sha256": {source.name: digest(frozen / source.name) for source in sources},
        "timeout_seconds": args.timeout,
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
    native_path = str(runtime / "lib/wine/x86_64-unix" / WINEMETAL_NATIVE_LIBRARY)
    native_paths = [native_path]
    native_loads = [line for line in dyld if any(path in line for path in native_paths)]
    # The owned DXIL compiler is inside this module, so a DXIL run only proves
    # the native compiler path when dyld loaded the staged module itself.
    native_module = runtime / "lib/wine/x86_64-unix" / WINEMETAL_NATIVE_LIBRARY
    native_compiler_proof = native_module.is_file() and any(native_path in line for line in dyld)
    prefix = SUITE_PREFIXES[args.suite]
    backend_prefix = BACKEND_PREFIXES.get(args.suite, prefix)
    marker_ok = (args.suite == "dxil-static-sampler"
                 or f"{backend_prefix}_BACKEND id=owned-graphics-d3d12" in stdout)
    frames = re.findall(r"OWNED_D3D12_SMOKE_FRAME frame=(\d+) readback=ok", stdout)
    barrier_ok = "OWNED_D3D12_SMOKE_BARRIER_STRESS readback=ok" in stdout
    success = (code == 0 and not timed_out and marker_ok and f"{prefix}_PASS" in stdout
               and native_loads and not foreign)
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
                   and "OWNED_D3D12_ENHANCED_BARRIERS_INTERFACES device14=1 command_list9=1 command_list10=0" in stdout
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
                                 r"lanes=32-32 total_lanes=\d+ int64=1 barycentrics=1 view_instancing=0 "
                                 r"native16=1 atomic64_typed=0 atomic64_groupshared=0 atomic64_heap=0 "
                                 r"wave_mma=0$", stdout, re.MULTILINE)
                   and f"OWNED_D3D12_SM6_PASS stress_reps={SM6_STRESS_REPS}" in stdout
                   and "OWNED_D3D12_SM6_MISMATCH" not in stdout
                   and "OWNED_D3D12_SM6_CASE_FAIL" not in stdout
                   and "OWNED_D3D12_SM6_FAIL" not in stdout + stderr)
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
        success = (success and stdout.count("OWNED_D3D12_STREAM_OUTPUT_VALIDATION_PASS cases=104") == 1
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
        matrix_cases = {"geometry": 41, "tessellation": 146}[args.suite]
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
    if args.suite == "temporal":
        frames = re.findall(r"OWNED_D3D12_TEMPORAL_FRAME_PASS variant=(\w+) frame=(\d+)\b", stdout)
        success = success and frames == [(variant, str(n)) for variant in ("typed", "typeless_zzz")
                                         for n in range(8)]
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
        success = (success and dxil_compiler_loaded and not raytracing_failures_found
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
    parser.add_argument("--lock", type=Path, default=DEFAULT_LOCK,
                        help="authoritative owned-build lock recorded as evidence for this run")
    parser.add_argument("--fixture", type=Path, required=True)
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
