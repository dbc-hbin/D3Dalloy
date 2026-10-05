#!/usr/bin/env python3
"""CPU-only synthetic protocol controls; these traces are not GPU execution evidence."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("owned_casting_runner", ROOT / "scripts/test-owned-graphics.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def complete_trace(suite: str, caps: int = 1) -> str:
    lines = [f"RELAXED_CASTING_OPTIONS12 relaxed={caps}"]
    cases = []
    volumes = []
    if suite == "relaxed-casting":
        # Independent DXGI numeric protocol, not imported from runner expectations.
        for placed in (0, 1):
            for base, target, bc, legacy, writable in (
                    (28, 42, 0, 0, 0), (11, 18, 0, 0, 0), (3, 2, 0, 0, 0),
                    (71, 17, 1, 0, 0), (98, 3, 1, 0, 0), (41, 41, 0, 1, 0),
                    (39, 42, 0, 1, 0), (13, 13, 0, 1, 0), (28, 42, 0, 0, 1),
                    (79, 81, 0, 1, 0), (71, 17, 1, 0, 1)):
                cases.append((base, target, bc, placed, legacy, writable, 0, 0, 2))
        cases.extend(((71, 17, 1, 0, 0, 0, 1, 0, 3), (71, 17, 1, 0, 0, 1, 1, 0, 2)))
        marker = "RELAXED_CASTING_PASS cases=24 GPU=exact"
        lines.append("RELAXED_CASTING_NEGATIVES nonBC_to_BC=reject planar=reject unequal_unit=reject")
    elif suite == "relaxed-casting-shared":
        cases = [(28, 42, 0, 0, 0, 0, 0, 1, 2), (28, 42, 0, 0, 0, 1, 0, 1, 2),
                 (71, 17, 1, 0, 0, 1, 0, 1, 2)]
        volumes = [(71, 8, 0, 1, 0, 1), (98, 16, 0, 1, 0, 1)]
        marker = "RELAXED_CASTING_SHARED_PASS cases=5 GPU=exact"
    else:
        standard = suite == "relaxed-casting-volume-standard"
        volumes = [(base, unit, placed, 0, 3 if standard else 0, writable)
                   for base, unit in ((71, 8), (98, 16)) for writable in (0, 1) for placed in (0, 1)]
        marker = f"RELAXED_CASTING_VOLUME{'_STANDARD' if standard else ''}_PASS cases=8 GPU=exact"
    for base, target, bc, placed, legacy, writable, sparse, shared, replay in cases:
        lines.append(f"RELAXED_CASTING_CASE base={base} target={target} bc={bc} placed={placed} "
                     f"legacy={legacy} writable={writable} sparse={sparse} shared={shared} "
                     f"mip=1 slice=1 words=exact replay={replay}")
    for base, unit, placed, shared, layout, writable in volumes:
        lines.append(f"RELAXED_CASTING_VOLUME_CASE base={base} unit={unit} placed={placed} shared={shared} layout={layout} "
                     f"writable={writable} alias={'same-dispatch-exact' if writable else 'none'} "
                     "mip=1 depth=32 words=exact nativeBC=exact nativecopy=exact replay=2")
    lines.append(marker)
    return "\n".join(lines) + "\n"


class CastingEvidenceTests(unittest.TestCase):
    def test_complete_all_modes(self) -> None:
        for suite in ("relaxed-casting", "relaxed-casting-shared",
                      "relaxed-casting-volume", "relaxed-casting-volume-standard"):
            trace = complete_trace(suite)
            with self.subTest(suite=suite):
                self.assertEqual(runner.relaxed_casting_failures(suite, trace), [])
                self.assertEqual(runner.BACKEND_PREFIXES.get(suite, runner.SUITE_PREFIXES[suite]),
                                 "RELAXED_CASTING")
                self.assertEqual(runner.suite_inputs(suite, ROOT / "unused.exe", ROOT),
                                 [ROOT / "graphics-translator/tests/owned-d3d12-relaxed-casting.cpp"])

    def test_every_line_missing_or_duplicated(self) -> None:
        for suite in ("relaxed-casting", "relaxed-casting-shared", "relaxed-casting-volume",
                      "relaxed-casting-volume-standard"):
            trace = complete_trace(suite)
            lines = trace.splitlines()
            for index, line in enumerate(lines):
                with self.subTest(suite=suite, line=line):
                    self.assertTrue(runner.relaxed_casting_failures(
                        suite, "\n".join(lines[:index] + lines[index + 1:])))
                    self.assertTrue(runner.relaxed_casting_failures(suite, trace + line + "\n"))

    def test_cap_only_and_wrong_native_numerics_rejected(self) -> None:
        suite = "relaxed-casting"
        trace = complete_trace(suite)
        for old, new in (("base=71", "base=98"), ("target=17", "target=42"),
                         ("words=exact", "words=wrong"), ("replay=3", "replay=2"),
                         ("sparse=1", "sparse=0"), ("writable=1", "writable=0")):
            with self.subTest(mutation=(old, new)):
                self.assertTrue(runner.relaxed_casting_failures(suite, trace.replace(old, new, 1)))
        self.assertTrue(runner.relaxed_casting_failures(suite, "RELAXED_CASTING_OPTIONS12 relaxed=1\n"
                                                      "RELAXED_CASTING_PASS cases=24 GPU=exact\n"))
        self.assertTrue(runner.relaxed_casting_failures(suite, trace.replace("relaxed=1", "relaxed=0")))
        self.assertTrue(runner.relaxed_casting_failures(suite, trace, "RELAXED_CASTING_FAIL step=GPU"))
        self.assertTrue(runner.relaxed_casting_failures(suite, trace + "RELAXED_CASTING_CASE malformed\n"))
        for suite in ("relaxed-casting-volume", "relaxed-casting-volume-standard", "relaxed-casting-shared"):
            trace = complete_trace(suite)
            for old, new in (("depth=32", "depth=1"), ("nativeBC=exact", "nativeBC=unchecked"),
                             ("nativecopy=exact", "nativecopy=unchecked"), ("layout=", "bad_layout="),
                             ("alias=same-dispatch-exact", "alias=none"), ("writable=1", "writable=0"),
                             ("unit=16", "unit=8"), ("base=98", "base=71")):
                self.assertTrue(runner.relaxed_casting_failures(suite, trace.replace(old, new, 1)))


if __name__ == "__main__":
    unittest.main()
