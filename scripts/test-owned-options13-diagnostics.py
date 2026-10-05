#!/usr/bin/env python3
"""CPU-only false-pass tests. These synthetic traces are not evidence of GPU execution."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("owned_options13_runner", ROOT / "scripts/test-owned-graphics.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def synthetic_complete_trace() -> str:
    lines = []
    # Independent fixture protocol, intentionally not imported from production expectations.
    for name in ("alpha_blend", "viewport_z", "viewport_y", "vertex_alignment", "copy_pitch",
                 "cross_dimension", "triangle_fan", "manual_write_tracking", "capabilities"):
        lines.append(f"OWNED_D3D12_OPTIONS13_CASE_BEGIN {name}")
        if name == "copy_pitch":
            for queue in (0, 2, 3):
                for format_name, size, dimensions in (
                    ("r8", 1, (1, 2, 3)), ("r16", 2, (1, 2, 3)), ("rgba8", 4, (1, 2, 3)),
                    ("rgba16", 8, (1, 2, 3)), ("rgba32", 16, (1, 2, 3)), ("rgb32", 12, (1, 2, 3)),
                    ("bc1", 8, (2,)), ("bc3", 16, (2,)), ("bc7", 16, (2,)),
                ):
                    for dimension in dimensions:
                        lines.append(f"OWNED_D3D12_OPTIONS13_COPY format={format_name} dims={dimension} "
                                     f"queue={queue} bytes={size} guards=1")
        elif name == "cross_dimension":
            for source, destination in ((1, 2), (1, 3), (2, 1), (2, 3), (3, 1), (3, 2)):
                lines.append(f"OWNED_D3D12_OPTIONS13_CROSS from={source} to={destination} offsets=1 guards=1")
            lines.append("OWNED_D3D12_OPTIONS13_CROSS_BOUNDS rejected=18 replay=6")
        elif name == "triangle_fan":
            rows = (("direct_empty", 0), ("direct_one", 0), ("direct_two", 0), ("direct_triangle", 1),
                    ("direct_start_instances", 8), ("direct_zero_instances", 0), ("indexed16_offset_base", 4),
                    ("indexed32_negative_base", 4), ("indexed16_restart", 4), ("indexed32_restart_gs", 4),
                    ("indexed16_cut_disabled", 1), ("indirect_draw", 8), ("indirect_indexed_count", 8),
                    ("bundle_direct_twice", 8), ("bundle_indexed16", 4), ("gs_input_order", 4),
                    ("backface_culled", 4), ("degenerate_count", 4))
            for row, primitives in rows:
                gs = primitives if row in ("indexed32_restart_gs", "gs_input_order") else 0
                lines.append(f"OWNED_D3D12_OPTIONS13_FAN row={row} pixels=64 colored=0 ia={primitives} gs={gs} samples=0 replay=1")
            lines.append("OWNED_D3D12_OPTIONS13_FAN_ORACLE rows=18 pixels=1152 primitives=66 cpu=1 winding=1 provoking=1 restart=1 indirect=1 bundles=1 gs=1 queries=1 occlusion=1 replay=1")
        elif name == "manual_write_tracking":
            lines.append("OWNED_D3D12_OPTIONS13_MANUAL_TRACKING public=0 committed_rejected=1 heap_rejected=1 tools_qi=absent")
            for queue in (0, 2, 3):
                lines.append(f"OWNED_D3D12_OPTIONS13_IMMEDIATE queue={queue} snapshots=3 bytes=192 modes=3 replay=1 changed_cpu_input=1 guards=1")
            lines.append("OWNED_D3D12_OPTIONS13_MANUAL_ORACLE public=0 rejected=2 tools_qi=absent immediate_queues=3 snapshots=9 replay=3 cpu=1")
        if name == "copy_pitch":
            lines.append("OWNED_D3D12_OPTIONS13_COPY_BOUNDS rejected=6 replay=63")
        lines.append(f"OWNED_D3D12_OPTIONS13_CASE_PASS {name}")
    lines.append("OWNED_D3D12_OPTIONS13_PASS cases=9")
    return "\n".join(lines) + "\n"


class Options13EvidenceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.trace = synthetic_complete_trace()

    def test_complete_protocol(self) -> None:
        self.assertEqual(runner.options13_failures(self.trace), [])

    def test_filtered_diagnostic_not_a_pass(self) -> None:
        trace = ("OWNED_D3D12_OPTIONS13_CASE_BEGIN alpha_blend\n"
                 "OWNED_D3D12_OPTIONS13_CASE_PASS alpha_blend\nOWNED_D3D12_OPTIONS13_PASS cases=1\n")
        self.assertTrue(runner.options13_failures(trace))

    def test_missing_each_line(self) -> None:
        lines = self.trace.splitlines()
        for index in range(len(lines)):
            with self.subTest(line=lines[index]):
                self.assertTrue(runner.options13_failures("\n".join(lines[:index] + lines[index + 1:])))

    def test_duplicate_each_line(self) -> None:
        for line in self.trace.splitlines():
            with self.subTest(line=line):
                self.assertTrue(runner.options13_failures(self.trace + line + "\n"))

    def test_wrong_pitch_unit_and_dimensions(self) -> None:
        for before, after in (("bytes=12", "bytes=4"), ("format=bc7", "format=rgba32"),
                              ("dims=3", "dims=2"), ("queue=3", "queue=0"),
                              ("guards=1", "guards=0"), ("offsets=1", "offsets=0")):
            with self.subTest(mutation=(before, after)):
                self.assertTrue(runner.options13_failures(self.trace.replace(before, after, 1)))

    def test_reordered_cases_and_copies(self) -> None:
        for a, b in (("alpha_blend", "viewport_z"), ("format=r8 ", "format=r16 ")):
            trace = self.trace.replace(a, "temporary-swap").replace(b, a).replace("temporary-swap", b)
            self.assertTrue(runner.options13_failures(trace))

    def test_render_replay_and_public_false_semantics(self) -> None:
        for before, after in (("rows=18", "rows=17"), ("pixels=1152", "pixels=64"),
                              ("primitives=66", "primitives=65"), ("public=0", "public=1"),
                              ("tools_qi=absent", "tools_qi=present"), ("rejected=18", "rejected=0"),
                              ("replay=63", "replay=1"), ("snapshots=9", "snapshots=3"),
                              ("changed_cpu_input=1", "changed_cpu_input=0"), ("occlusion=1", "occlusion=0")):
            with self.subTest(mutation=(before, after)):
                self.assertTrue(runner.options13_failures(self.trace.replace(before, after, 1)))

    def test_failure_on_either_stream(self) -> None:
        error = "OWNED_D3D12_OPTIONS13_FAIL step=oracle hr=80004005\n"
        self.assertTrue(runner.options13_failures(self.trace + error))
        self.assertTrue(runner.options13_failures(self.trace, error))

if __name__ == "__main__":
    unittest.main()
