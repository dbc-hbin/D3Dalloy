#!/usr/bin/env python3
"""Reject false-positive GPU reports using controlled failing traces; no GPU/Wine execution."""
from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT / "build/local/diagnostics-cpu"


def load(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runner = load("owned_runner", "test-owned-graphics.py")
analyzer = load("owned_trace_analyzer", "analyze-owned-temporal-trace.py")


class RunnerFailures(unittest.TestCase):
    def simulate(self, *, omit_frame: int | None = None, backend_log: str = "",
                 foreign: bool = False, timeout: bool = False, stderr_diagnostic: str = "") -> dict:
        with tempfile.TemporaryDirectory(dir=SCRATCH, prefix="runner-") as directory:
            base = Path(directory)
            runtime = base / "runtime"
            runtime.mkdir()
            (runtime / "owned-graphics-runtime.json").write_text("{}\n")
            fixture = base / "owned-d3d12-ngx.exe"
            fixture.write_bytes(b"controlled runner fixture, never executed")
            args = argparse.Namespace(runtime=runtime, lock=runtime / "owned-graphics-runtime.json",
                                      fixture=fixture, dxil_dir=base / "dxil", out=base / "result",
                                      suite="ngx", mode="no-present", validation=True, timeout=1)

            def fake_run(command, **kwargs):
                if command[0] == str(runtime / "bin/wine"):
                    # The NGX PE deliberately reuses the temporal backend guard.
                    kwargs["stdout"].write("OWNED_D3D12_TEMPORAL_BACKEND id=owned-graphics-d3d12\n")
                    for frame in range(8):
                        if frame != omit_frame:
                            kwargs["stdout"].write(f"OWNED_D3D12_NGX_FRAME_PASS frame={frame}\n")
                    kwargs["stdout"].write("OWNED_D3D12_NGX_PASS\n")
                    kwargs["stderr"].write(f"dyld[1]: {runtime}/lib/wine/x86_64-unix/winemetal.so\n")
                    kwargs["stderr"].write(stderr_diagnostic)
                    if foreign:
                        kwargs["stderr"].write("dyld[1]: /not-executed/D3DMetal.framework/D3DMetal\n")
                    (args.out / "owned-d3d12-ngx_d3d12.log").write_text(backend_log)
                    if timeout:
                        raise subprocess.TimeoutExpired(command, 1)
                return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

            with patch.object(runner.subprocess, "run", side_effect=fake_run), contextlib.redirect_stdout(io.StringIO()):
                result = runner.run(args)
            report = json.loads((args.out / "result.json").read_text())
            self.assertEqual(result, 0 if report["pass"] else 1)
            return report

    def simulate_dxil(self, *, shaders: bool = True, compute_actual: str = "0x13579bdf",
                      native: bool = True, loaded: bool = True, table_expected: str = "0x2468ace0",
                      table_actual: str = "0x2468ace0", table_line: bool = True,
                      unsupported_table: bool = False, depth: str = "0.000000",
                      space_line: str = "b0_space2=0x13579bdf b0_space9=0x2468ace0") -> dict:
        with tempfile.TemporaryDirectory(dir=SCRATCH, prefix="dxil-") as directory:
            base = Path(directory)
            runtime = base / "runtime"
            (runtime / "lib/wine/x86_64-unix").mkdir(parents=True)
            (runtime / "owned-graphics-runtime.json").write_text("{}\n")
            if native:
                (runtime / "lib/wine/x86_64-unix/winemetal.so").write_bytes(b"native")
            fixture = base / "owned-d3d12-dxil.exe"
            fixture.write_bytes(b"controlled runner fixture, never executed")
            dxil = base / "dxil"
            dxil.mkdir()
            if shaders:
                for name in runner.DXIL_SHADERS + runner.DXIL_REGRESSION_SHADERS:
                    (dxil / name).write_bytes(b"dxil-" + name.encode())
            args = argparse.Namespace(runtime=runtime, lock=runtime / "owned-graphics-runtime.json",
                                      fixture=fixture, dxil_dir=dxil, out=base / "result",
                                      suite="dxil", mode="no-present", validation=False, timeout=1)

            def fake_run(command, **kwargs):
                if command[0] == str(runtime / "bin/wine"):
                    kwargs["stdout"].write("OWNED_D3D12_DXIL_BACKEND id=owned-graphics-d3d12\n")
                    kwargs["stdout"].write(
                        f"OWNED_D3D12_DXIL_COMPUTE index=3 expected=0x13579bdf actual={compute_actual}\n")
                    if table_line:
                        kwargs["stdout"].write(f"{runner.DXIL_DESCRIPTOR_TABLE_MARKER} "
                                               f"expected={table_expected} actual={table_actual}\n")
                    if unsupported_table:
                        kwargs["stdout"].write("OWNED_D3D12_DXIL_DESCRIPTOR_TABLE hr=0x80004001\n")
                    if space_line:
                        kwargs["stdout"].write(f"OWNED_D3D12_DXIL_REGISTER_SPACES {space_line}\n")
                    for slot in range(4):
                        kwargs["stdout"].write(f"OWNED_D3D12_DXIL_VARYING slot={slot} "
                                               "value=(1.0,2.0,1,1) expected=(1,2,1,1)\n")
                    kwargs["stdout"].write(f"OWNED_D3D12_DXIL_DEPTH center={depth} expected=0.0 rows=8 row_bytes=32\n")
                    kwargs["stdout"].write("OWNED_D3D12_DXIL_PASS\n")
                    if loaded:
                        kwargs["stderr"].write(f"dyld[1]: {runtime}/lib/wine/x86_64-unix/winemetal.so\n")
                return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

            with patch.object(runner.subprocess, "run", side_effect=fake_run), \
                    contextlib.redirect_stdout(io.StringIO()):
                result = runner.run(args)
            report = json.loads((args.out / "result.json").read_text())
            self.assertEqual(result, 0 if report["pass"] else 1)
            return report

    def test_valid_dxil_run_passes_and_freezes_shader_argv(self):
        report = self.simulate_dxil()
        self.assertTrue(report["pass"])
        self.assertIn("compute.dxil", report["fixture_sha256"])
        self.assertEqual(report["command"][-3:], list(runner.DXIL_SHADERS))
        # The owned compiler lives inside the staged native module: the proof is
        # that exact module loading, never a separately licensed SDK dylib.
        self.assertTrue(report["dxil_native_compiler_verified"])
        self.assertEqual([Path(line.split(": ", 1)[1]).name for line in report["owned_native_loads"]],
                         ["winemetal.so"])

    def test_dxil_without_frozen_shader_inputs_cannot_run(self):
        with self.assertRaisesRegex(ValueError, "regular file"):
            self.simulate_dxil(shaders=False)

    def test_dxil_compute_mismatch_cannot_pass(self):
        self.assertFalse(self.simulate_dxil(compute_actual="0x00000000")["pass"])

    def test_dxil_aliased_register_spaces_cannot_pass(self):
        self.assertFalse(self.simulate_dxil(space_line="b0_space2=0x2468ace0 b0_space9=0x2468ace0")["pass"])

    def test_dxil_stale_fixture_without_register_spaces_cannot_pass(self):
        self.assertFalse(self.simulate_dxil(space_line="")["pass"])

    def test_dxil_descriptor_table_must_execute_expected_values(self):
        self.assertFalse(self.simulate_dxil(table_actual="0x00000000")["pass"])

    def test_dxil_descriptor_table_equal_but_unapproved_value_cannot_pass(self):
        self.assertFalse(self.simulate_dxil(table_expected="0x11111111",
                                            table_actual="0x11111111")["pass"])

    def test_dxil_missing_descriptor_table_marker_cannot_pass(self):
        self.assertFalse(self.simulate_dxil(table_line=False)["pass"])

    def test_dxil_e_notimpl_descriptor_table_cannot_pass(self):
        self.assertFalse(self.simulate_dxil(table_line=False, unsupported_table=True)["pass"])

    def test_dxil_wrong_depth_cannot_pass(self):
        self.assertFalse(self.simulate_dxil(depth="1.000000")["pass"])

    def test_dxil_without_the_owned_native_module_cannot_pass(self):
        self.assertFalse(self.simulate_dxil(native=False)["pass"])

    def test_dxil_owned_native_module_must_actually_load(self):
        self.assertFalse(self.simulate_dxil(loaded=False)["pass"])

    def simulate_raytracing(self, *, shaders: bool = True, lanes: bool = True,
                            loaded: bool = True, fill_lanes: bool = False,
                            blas_prebuild: tuple[int, int, int] = (1024, 512, 256),
                            admissible_variant: int = 0, procedural: bool = True,
                            procedural_hit_group: int | None = None,
                            procedural_any_hit_on_rejected: bool = False,
                            procedural_untightened_interval: bool = False,
                            procedural_clobbered_commit: bool = False) -> dict:
        with tempfile.TemporaryDirectory(dir=SCRATCH, prefix="raytracing-") as directory:
            base = Path(directory)
            runtime = base / "runtime"
            (runtime / "lib/wine/x86_64-unix").mkdir(parents=True)
            (runtime / "owned-graphics-runtime.json").write_text("{}\n")
            (runtime / "lib/wine/x86_64-unix/winemetal.so").write_bytes(b"native")
            fixture = base / "owned-d3d12-raytracing.exe"
            fixture.write_bytes(b"controlled runner fixture, never executed")
            dxil = base / "dxil"
            dxil.mkdir()
            if shaders:
                for name in runner.RAYTRACING_SHADERS:
                    (dxil / name).write_bytes(b"dxil-" + name.encode())
            args = argparse.Namespace(runtime=runtime, lock=runtime / "owned-graphics-runtime.json",
                                      fixture=fixture, dxil_dir=dxil, out=base / "result",
                                      suite="raytracing", mode="no-present", validation=False, timeout=1)

            def fake_run(command, **kwargs):
                if command[0] == str(runtime / "bin/wine"):
                    out = kwargs["stdout"]
                    out.write("OWNED_D3D12_RAYTRACING_BACKEND id=owned-graphics-d3d12\n")
                    out.write("OWNED_D3D12_RAYTRACING_PREBUILD blas_max=%d blas_scratch=%d "
                              "blas_update_scratch=%d\n" % blas_prebuild)
                    out.write("OWNED_D3D12_RAYTRACING_PREBUILD_TLAS tlas_max=2048 tlas_scratch=1024\n")
                    out.write("OWNED_D3D12_RAYTRACING_PREBUILD_AABB aabb_max=1024 aabb_scratch=512\n")
                    if lanes:
                        for index, expected in enumerate(runner.RAYTRACING_EXPECTATIONS):
                            variant = expected.variants[min(admissible_variant,
                                                             len(expected.variants) - 1)]
                            if fill_lanes:
                                # What a dispatch that never traced anything leaves behind:
                                # the payload and every shader log word stay at the fill pattern.
                                head = (runner.RAYTRACING_FILL, runner.RAYTRACING_FILL,
                                        runner.RAYTRACING_FILL, 0.0, 0, 0)
                                bary = (0.0, 0.0)
                                tail = (0, 0, 0, 0, 0)
                                logs = (runner.RAYTRACING_FILL,) * 4
                                shader_logs = (runner.RAYTRACING_FILL,) * 3
                            else:
                                head = (expected.kind, variant.primitive, expected.instance,
                                        variant.hit_t, expected.callable, expected.local_root)
                                bary = (variant.bary_u, variant.bary_v)
                                tail = (expected.depth, expected.recurse, expected.nested_miss,
                                        variant.any_hit_tag, variant.reject_tag)
                                logs = variant.any_hit_log
                                shader_logs = (expected.closest_hit_log, expected.miss_log,
                                               expected.callable_log)
                            out.write("OWNED_D3D12_RAYTRACING_LANE lane=%d kind=%d primitive=%d "
                                      "instance=%d hit_t=%.6f callable=0x%08x local_root=0x%08x "
                                      "lane_field=%d bary=(%.6f,%.6f) dimensions=%d depth=%d "
                                      "recurse=%d nested_miss=%d anyhit_tag=0x%08x reject_tag=0x%08x\n"
                                      % (index, *head, index, *bary, runner.RAYTRACING_LANE_COUNT, *tail))
                            out.write("OWNED_D3D12_RAYTRACING_LOG lane=%d "
                                      "anyhit=(0x%08x,0x%08x,0x%08x,0x%08x) closesthit=0x%08x "
                                      "miss=0x%08x callable=0x%08x\n"
                                      % (index, *logs, *shader_logs))
                    if procedural:
                        for index, expected in enumerate(runner.RAYTRACING_PROCEDURAL_EXPECTATIONS):
                            if fill_lanes:
                                # What a procedural dispatch that never traced anything leaves
                                # behind: every payload word and every log word stays at the fill
                                # pattern, which is not any lane's admissible outcome.
                                payload = (runner.RAYTRACING_FILL, runner.RAYTRACING_FILL,
                                           runner.RAYTRACING_FILL, 0, runner.RAYTRACING_FILL,
                                           runner.RAYTRACING_FILL, runner.RAYTRACING_FILL,
                                           0, 0, runner.RAYTRACING_FILL, runner.RAYTRACING_FILL,
                                           runner.RAYTRACING_FILL, runner.RAYTRACING_FILL,
                                           runner.RAYTRACING_FILL, runner.RAYTRACING_FILL)
                                intersection_log = (runner.RAYTRACING_FILL,) * (
                                    runner.RAYTRACING_PROCEDURAL_CROSSINGS)
                                any_hit_log = (runner.RAYTRACING_FILL,) * (
                                    runner.RAYTRACING_PROCEDURAL_CROSSINGS
                                    * runner.RAYTRACING_PROCEDURAL_ANY_HIT_CROSSING_WORDS)
                                stage_logs = (runner.RAYTRACING_FILL, runner.RAYTRACING_FILL)
                                hit_log = (runner.RAYTRACING_FILL,) * 8
                            else:
                                local_root = (expected.local_root if procedural_hit_group is None
                                              else procedural_hit_group)
                                any_hit_log = expected.any_hit_log
                                if (procedural_any_hit_on_rejected
                                        and any_hit_log == runner.RAYTRACING_PROCEDURAL_NO_ANY_HIT):
                                    # The report was rejected by DXR before the any-hit shader ran,
                                    # so an any-hit log here is a shader that ran when it must not.
                                    any_hit_log = runner.raytracing_procedural_any_hit_log(
                                        (runner.raytracing_procedural_any_hit_slot(
                                            index, False, runner.RAYTRACING_HALF_BITS),
                                         runner.RAYTRACING_PROCEDURAL_NO_ANY_HIT_SLOT,
                                         runner.RAYTRACING_PROCEDURAL_NO_ANY_HIT_SLOT))
                                intersection_log = expected.intersection_log
                                hit_log = expected.hit_log
                                if procedural_untightened_interval and index == (
                                        runner.RAYTRACING_PROCEDURAL_LANE_COUNT - 1):
                                    # A runtime that kept testing later reports against the
                                    # untightened TMax would accept the farther report and run its
                                    # any-hit: the intersection word flips to accepted/in-interval
                                    # and the slot stops being the fill pattern.
                                    crossing = runner.RAYTRACING_MULTI_REPORT_TIGHTENED_CROSSING
                                    intersection_log = tuple(
                                        runner.raytracing_intersection_log(
                                            runner.RAYTRACING_MULTI_REPORT_PRIMITIVE, True, True)
                                        if word_index == crossing else word
                                        for word_index, word in enumerate(intersection_log))
                                    slots = [list(any_hit_log[slot * 4:(slot + 1) * 4])
                                             for slot in range(runner.RAYTRACING_PROCEDURAL_CROSSINGS)]
                                    slots[crossing] = list(runner.raytracing_procedural_any_hit_slot(
                                        runner.RAYTRACING_MULTI_REPORT_PRIMITIVE, False,
                                        runner.RAYTRACING_MULTI_REPORT_ENTRY1_BITS))
                                    any_hit_log = tuple(word for slot in slots for word in slot)
                                if procedural_clobbered_commit and index == (
                                        runner.RAYTRACING_PROCEDURAL_LANE_COUNT - 1):
                                    # A runtime that let the later rejected report overwrite the
                                    # committed hit would report crossing 2's distance, attributes
                                    # and ray t in the closest-hit log.
                                    hit_log = tuple(
                                        runner.RAYTRACING_MULTI_REPORT_ENTRY2_BITS
                                        if word_index in (1, 2) else word
                                        for word_index, word in enumerate(hit_log))
                                payload = (expected.kind, expected.primitive, expected.instance,
                                           expected.hit_t_bits, 0, local_root, index, 0.0, 0.0,
                                           runner.RAYTRACING_PROCEDURAL_LANE_COUNT, 0, 0, 0,
                                           expected.any_hit_tag, expected.reject_tag)
                                stage_logs = (expected.closest_hit_log, expected.miss_log)
                            hit_t = struct.unpack(">f", struct.pack(">I", payload[3]))[0]
                            out.write("OWNED_D3D12_RAYTRACING_PROCEDURAL lane=%d kind=%d primitive=%d "
                                      "instance=%d hit_t=%.6f local_root=0x%08x lane_field=%d "
                                      "bary=(%.6f,%.6f) dimensions=%d anyhit_tag=0x%08x reject_tag=0x%08x\n"
                                      % (index, payload[0], payload[1], payload[2], hit_t, payload[5],
                                         index, payload[7], payload[8], payload[9], payload[13],
                                         payload[14]))
                            out.write("OWNED_D3D12_RAYTRACING_PROCEDURAL_LOG lane=%d "
                                      "intersection=(0x%08x,0x%08x,0x%08x) "
                                      "anyhit=(0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,"
                                      "0x%08x,0x%08x,0x%08x,0x%08x,0x%08x) "
                                      "closesthit=0x%08x miss=0x%08x "
                                      "hit=(0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x)\n"
                                      % (index, *intersection_log, *any_hit_log, *stage_logs, *hit_log))
                    out.write(f"{runner.RAYTRACING_PASS_MARKER}\n")
                    if loaded:
                        kwargs["stderr"].write(f"dyld[1]: {runtime}/lib/wine/x86_64-unix/winemetal.so\n")
                return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

            with patch.object(runner.subprocess, "run", side_effect=fake_run), \
                    contextlib.redirect_stdout(io.StringIO()):
                result = runner.run(args)
            report = json.loads((args.out / "result.json").read_text())
            self.assertEqual(result, 0 if report["pass"] else 1)
            return report

    def test_valid_raytracing_run_passes_and_freezes_shader_argv(self):
        report = self.simulate_raytracing()
        self.assertTrue(report["pass"], report["raytracing"])
        # The DXR library is frozen beside the executable and passed as argv, so the
        # evidence records exactly which shader the dispatch was traced with.
        self.assertIn("raytracing.dxil", report["fixture_sha256"])
        self.assertEqual(report["command"][-1], "raytracing.dxil")
        self.assertTrue(report["dxil_native_compiler_verified"])
        self.assertEqual(report["raytracing"]["lanes"], list(range(runner.RAYTRACING_LANE_COUNT)))

    def test_raytracing_without_frozen_shader_input_cannot_run(self):
        with self.assertRaisesRegex(ValueError, "regular file"):
            self.simulate_raytracing(shaders=False)

    def test_raytracing_pass_line_without_lane_readback_cannot_pass(self):
        report = self.simulate_raytracing(lanes=False)
        self.assertFalse(report["pass"])
        self.assertTrue(report["raytracing"]["failures"])

    def test_raytracing_dispatch_that_traced_nothing_cannot_pass(self):
        # Every lane left at the fill pattern is what a Tier-0/no-op dispatch produces:
        # the PASS line is printed but no shader ever wrote the payload.
        report = self.simulate_raytracing(fill_lanes=True)
        self.assertFalse(report["pass"])
        failures = report["raytracing"]["failures"]
        for lane in range(runner.RAYTRACING_LANE_COUNT):
            self.assertTrue(any(f"lane {lane} " in failure for failure in failures),
                            f"lane {lane} produced no failure evidence: {failures}")

    def test_raytracing_without_the_owned_native_module_cannot_pass(self):
        self.assertFalse(self.simulate_raytracing(loaded=False)["pass"])

    def test_raytracing_zero_update_scratch_can_pass(self):
        # ALLOW_UPDATE is not requested, so the runtime reports no update scratch at all; a
        # zero update-scratch size is a complete, honest prebuild record, not missing evidence.
        report = self.simulate_raytracing(blas_prebuild=(1024, 512, 0))
        self.assertTrue(report["pass"], report["raytracing"])

    def test_raytracing_zero_result_size_cannot_pass(self):
        report = self.simulate_raytracing(blas_prebuild=(0, 512, 0))
        self.assertFalse(report["pass"])
        self.assertTrue(report["raytracing"]["failures"])

    def test_raytracing_zero_initial_build_scratch_cannot_pass(self):
        report = self.simulate_raytracing(blas_prebuild=(1024, 0, 0))
        self.assertFalse(report["pass"])
        self.assertTrue(report["raytracing"]["failures"])

    def test_raytracing_both_admissible_candidate_orders_can_pass(self):
        # DXR leaves the candidate enumeration order unspecified, so the fixture lists every
        # admissible outcome and the runner must accept each of them rather than only the
        # first: failing a legitimate runtime over an order DXR never promised is the
        # mirror-image false positive of a false PASS.
        report = self.simulate_raytracing(admissible_variant=1)
        self.assertTrue(report["pass"], report["raytracing"])

    def test_valid_raytracing_run_records_procedural_lanes(self):
        # The procedural dispatch is part of the same contract: a run whose procedural lanes are
        # absent is not the run the fixture claims to have made.
        report = self.simulate_raytracing()
        self.assertTrue(report["pass"], report["raytracing"])
        self.assertEqual(report["raytracing"]["procedural_lanes"],
                         list(range(runner.RAYTRACING_PROCEDURAL_LANE_COUNT)))

    def test_raytracing_without_the_procedural_dispatch_cannot_pass(self):
        # The fixture's PASS line is not the evidence; the per-lane procedural readback is.
        report = self.simulate_raytracing(procedural=False)
        self.assertFalse(report["pass"])
        self.assertTrue(report["raytracing"]["failures"])

    def test_raytracing_procedural_hit_group_that_ignores_the_instance_contribution_cannot_pass(self):
        # A runtime that dropped InstanceContributionToHitGroupIndex from the hit group index
        # would run the TRIANGLE hit group of the table base for a procedural hit, so the
        # procedural lane's local_root would carry the triangle record's tag.
        report = self.simulate_raytracing(procedural_hit_group=runner.RAYTRACING_FIRST_HIT_GROUP)
        self.assertFalse(report["pass"])
        self.assertTrue(any("local_root" in failure or "payload mismatch" in failure
                            for failure in report["raytracing"]["failures"]),
                        report["raytracing"]["failures"])

    def test_raytracing_procedural_any_hit_that_runs_on_a_rejected_report_cannot_pass(self):
        # DXR rejects a report whose distance is outside the ray's own interval before the
        # any-hit shader runs at all, so an any-hit invocation for that candidate is a violation
        # the fill pattern in its log slot exists to catch.
        report = self.simulate_raytracing(procedural_any_hit_on_rejected=True)
        self.assertFalse(report["pass"])
        self.assertTrue(any("any-hit log" in failure for failure in report["raytracing"]["failures"]),
                        report["raytracing"]["failures"])

    def test_raytracing_procedural_report_beyond_the_tightened_interval_cannot_pass(self):
        # One intersection-shader invocation reports three times: t=1.0 accepted, then t=5.0 which
        # is farther than the committed hit. A runtime that kept testing later reports against the
        # untightened TMax would accept that report and run its any-hit shader, which the
        # intersection log's accept bit and the any-hit slot's fill pattern both catch.
        report = self.simulate_raytracing(procedural_untightened_interval=True)
        self.assertFalse(report["pass"])
        failures = report["raytracing"]["failures"]
        self.assertTrue(any("intersection log" in failure for failure in failures), failures)
        self.assertTrue(any("any-hit log" in failure for failure in failures), failures)

    def test_raytracing_procedural_rejected_report_that_clobbers_the_commit_cannot_pass(self):
        # The multi-report lane's third report is nearer than the committed hit and is rejected by
        # the any-hit shader. A runtime that let that rejected report overwrite the committed hit
        # would report the nearer distance, attributes and ray t in the closest-hit log, while the
        # committed hit must stay the first accepted report's.
        report = self.simulate_raytracing(procedural_clobbered_commit=True)
        self.assertFalse(report["pass"])
        self.assertTrue(any("closest-hit log" in failure for failure in report["raytracing"]["failures"]),
                        report["raytracing"]["failures"])

    def test_valid_ngx_uses_shared_backend_marker(self):
        self.assertTrue(self.simulate()["pass"])

    def test_missing_frame_cannot_pass(self):
        self.assertFalse(self.simulate(omit_frame=4)["pass"])

    def test_error_only_in_backend_file_cannot_pass(self):
        report = self.simulate(backend_log="err: Device error: Metal command buffer failed\n")
        self.assertFalse(report["pass"])
        self.assertTrue(report["validation_errors"])

    def test_shader_invalid_load_with_complete_readback_cannot_pass(self):
        report = self.simulate(stderr_diagnostic=
            "Invalid Metal usage: Invalid load in 'device' or 'constant' address space\n")
        self.assertFalse(report["pass"])
        self.assertTrue(report["validation_errors"])

    def test_failed_completion_without_error_text_cannot_pass(self):
        report = self.simulate(backend_log='info: OWNED_GRAPHICS_TRACE {"event":"complete","status":5}\n')
        self.assertFalse(report["pass"])

    def test_malformed_trace_cannot_pass(self):
        self.assertFalse(self.simulate(backend_log="OWNED_GRAPHICS_TRACE {broken\n")["pass"])

    def test_foreign_backend_cannot_pass(self):
        self.assertFalse(self.simulate(foreign=True)["pass"])

    def test_timeout_after_printed_pass_cannot_pass(self):
        self.assertFalse(self.simulate(timeout=True)["pass"])


def example_trace() -> list[dict]:
    events = []

    def add(kind: str, **fields) -> None:
        events.append({"event_id": len(events) + 1, "event": kind, **fields})

    for evaluation in (1, 2):
        add("temporal_record", evaluation=evaluation, feature=3, generation=4,
            color=10 + evaluation, output=20 + evaluation)
    add("record_begin", list=55, recording=1)
    add("wait_defer", queue=2, fence=80, generation=1, value=1)
    for recording in range(2, 98):
        add("record_begin", list=55, recording=recording)
    add("execute_list", queue=1, submission=10, list=54, recording=1)
    add("temporal_encode", queue=1, submission=10, evaluation=1, feature=3, generation=4,
        color=11, output=21, history_sequence=1, reset=1, caller_reset=0)
    add("submit", queue=1, submission=10)
    add("submit", queue=1, submission=11)
    add("signal_committed", queue=1, submission=11, fence=80, generation=1, value=1)
    add("wait", queue=2, fence=80, generation=1, value=1)
    add("execute_list", queue=2, submission=20, list=55, recording=1)
    add("temporal_encode", queue=2, submission=20, evaluation=2, feature=3, generation=4,
        color=12, output=22, history_sequence=2, reset=0, caller_reset=0)
    add("submit", queue=2, submission=20)
    add("complete", queue=1, submission=10, status=4)
    add("complete", queue=1, submission=11, status=4)
    add("complete", queue=2, submission=20, status=4)
    add("temporal_frame_destroy", evaluation=1)
    add("temporal_frame_destroy", evaluation=2)
    return events


class TemporalFailures(unittest.TestCase):
    def setUp(self):
        self.events = example_trace()

    def report(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH, prefix="trace-") as directory:
            path = Path(directory) / "trace.log"
            # Thread logger lines may arrive out of order; the IDs remain causal.
            path.write_text("".join("info: OWNED_GRAPHICS_TRACE " + json.dumps(event) + "\n"
                                    for event in reversed(self.events)))
            return analyzer.analyze(path)

    def test_valid_recording_replay_and_async_queue_order(self):
        report = self.report()
        self.assertTrue(report["pass"], report["errors"])
        self.assertTrue(report["crossqueue"][0]["consumer_encoded_before_producer_completion"])

    def test_history_reset_regression_rejected(self):
        next(e for e in self.events if e["event"] == "temporal_encode" and e["evaluation"] == 2)["reset"] = 1
        self.assertFalse(self.report()["pass"])

    def test_resource_changed_between_record_and_execute_rejected(self):
        next(e for e in self.events if e["event"] == "temporal_encode")["output"] = 999
        self.assertFalse(self.report()["pass"])

    def test_missing_producer_commit_rejected(self):
        self.events = [e for e in self.events if e["event"] != "signal_committed"]
        self.assertFalse(self.report()["pass"])

    def test_early_allocator_frame_destruction_rejected(self):
        destroy = next(e for e in self.events if e["event"] == "temporal_frame_destroy" and e["evaluation"] == 2)
        complete = next(e for e in self.events if e["event"] == "complete" and e["queue"] == 2)
        destroy["event_id"], complete["event_id"] = complete["event_id"], destroy["event_id"]
        self.assertFalse(self.report()["pass"])

    def test_leaked_recorded_frame_rejected(self):
        self.events = [e for e in self.events if not (e["event"] == "temporal_frame_destroy" and e["evaluation"] == 2)]
        self.assertFalse(self.report()["pass"])

    def test_failed_native_submission_rejected(self):
        next(e for e in self.events if e["event"] == "complete")["status"] = 5
        self.assertFalse(self.report()["pass"])


if __name__ == "__main__":
    SCRATCH.mkdir(parents=True, exist_ok=True)
    unittest.main(verbosity=2)
