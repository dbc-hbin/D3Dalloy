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


def controlled_pe_fixture() -> bytes:
    image = bytearray(0x200)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x40)
    image[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<H", image, 0x44, 0x8664)
    struct.pack_into("<H", image, 0x54, 240)
    struct.pack_into("<H", image, 0x58, 0x20B)
    return bytes(image)


def load(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runner = load("owned_runner", "test-owned-graphics.py")
analyzer = load("owned_trace_analyzer", "analyze-owned-temporal-trace.py")

# Production evidence from p3/dxrcover/run-03 and p3/mesh2/rt-01, plus the
# AS/all-stage RayQuery contracts. The geometry-transform oracle comes from
# build/local/metal4/recovery/integration/runtime01-ray64-01. Keep this independent
# of runner expectations so a newly required gate cannot manufacture passing evidence.
# HitObject marker: observed in p3-20261003/hitobject/full-runtime02-{x64,x86}/consumer.log.
RAYTRACING_CONTRACT_TRACE = """
OWNED_D3D12_RAYTRACING_FORMAT r32g32b32_float status=PASS
OWNED_D3D12_RAYTRACING_FORMAT r32g32_float status=PASS
OWNED_D3D12_RAYTRACING_FORMAT r16g16_float status=PASS
OWNED_D3D12_RAYTRACING_FORMAT r16g16b16a16_float status=PASS
OWNED_D3D12_RAYTRACING_FORMAT r16g16_snorm status=PASS
OWNED_D3D12_RAYTRACING_FORMAT r16g16b16a16_snorm status=PASS
OWNED_D3D12_RAYTRACING_FORMAT r16g16b16a16_unorm status=PASS
OWNED_D3D12_RAYTRACING_FORMAT r16g16_unorm status=PASS
OWNED_D3D12_RAYTRACING_FORMAT r10g10b10a2_unorm status=PASS
OWNED_D3D12_RAYTRACING_FORMAT r8g8b8a8_unorm status=PASS
OWNED_D3D12_RAYTRACING_FORMAT r8g8_unorm status=PASS
OWNED_D3D12_RAYTRACING_FORMAT r8g8b8a8_snorm status=PASS
OWNED_D3D12_RAYTRACING_FORMAT r8g8_snorm status=PASS
OWNED_D3D12_RAYTRACING_CASE vertex-formats status=PASS
OWNED_D3D12_RAYTRACING_CASE geometry-transform-row-major status=PASS
OWNED_D3D12_RAYTRACING_CASE geometry-rules status=PASS
OWNED_D3D12_RAYTRACING_CASE transforms status=PASS
OWNED_D3D12_RAYTRACING_CASE instances-hit-groups status=PASS
OWNED_D3D12_RAYTRACING_CONTRACT failures=0
RAYTRACING_AS case=postbuild-inline failures=0 status=PASS
RAYTRACING_AS case=postbuild-emit failures=0 status=PASS
RAYTRACING_AS case=clone-blas failures=0 status=PASS
RAYTRACING_AS case=clone-tlas failures=0 status=PASS
RAYTRACING_AS case=compact-blas failures=0 status=PASS
RAYTRACING_AS case=compact-tlas failures=0 status=PASS
RAYTRACING_AS case=serialize-blas failures=0 status=PASS
RAYTRACING_AS case=serialize-tlas-relocate failures=0 status=PASS
RAYTRACING_AS case=serialize-aabb failures=0 status=PASS
RAYTRACING_AS case=serialize-empty failures=0 status=PASS
RAYTRACING_AS case=visualize-blas failures=0 status=PASS
RAYTRACING_AS case=visualize-tlas failures=0 status=PASS
RAYTRACING_AS case=update-blas-inplace failures=0 status=PASS
RAYTRACING_AS case=update-blas-copy failures=0 status=PASS
RAYTRACING_AS case=update-tlas-inplace failures=0 status=PASS
RAYTRACING_AS case=update-tlas-copy failures=0 status=PASS
RAYTRACING_AS_SUMMARY cases=16 failures=0
OWNED_D3D12_RAYTRACING_CASE dxil-subobjects status=PASS
OWNED_D3D12_RAYTRACING_CASE config-flags status=PASS
OWNED_D3D12_RAYTRACING_CASE recursion-depth status=PASS
OWNED_D3D12_RAYTRACING_CASE stack-size status=PASS
OWNED_D3D12_RAYTRACING_CASE bundle status=PASS
OWNED_D3D12_RAYTRACING_CASE miss-ray-values status=PASS
OWNED_D3D12_RAYTRACING_CASE geometry-skip-flags status=PASS
OWNED_D3D12_RAYTRACING_CASE add-to-state-object status=PASS
OWNED_D3D12_RAYTRACING_STATEOBJ failures=0
OWNED_D3D12_RAYTRACING_CASE generic-function-collection status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE cs lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE vs lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE ps lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE raygen lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE raygen-indirect lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE closesthit lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE anyhit lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE intersection lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE miss lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE callable lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE hs lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE ds lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE gs lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE as lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_CASE ms lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_RAYQUERY_STAGES failures=0
OWNED_D3D12_RAYTRACING_RAYQUERY failures=0
OWNED_D3D12_RAYTRACING_ROOTARGS_CASE dispatch-rays-root-arguments slots=4 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_FEATURE geometry-index lanes=24 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_FEATURE hit-group-indexing lanes=8 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_FEATURE miss-index lanes=8 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_FEATURE traceray-skip-flags lanes=12 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_FEATURE callable-chains lanes=8 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_FEATURE rayquery-template-skip lanes=4 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_CASE grid-direct status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_CASE grid-indirect status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_PREDICATION predication-direct-eq0-0 culled=1 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_PREDICATION predication-indirect-eq0-0 culled=1 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_PREDICATION predication-direct-eq0-1 culled=0 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_PREDICATION predication-indirect-eq0-1 culled=0 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_PREDICATION predication-direct-ne0-0 culled=0 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_PREDICATION predication-indirect-ne0-0 culled=0 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_PREDICATION predication-direct-ne0-1 culled=1 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_PREDICATION predication-indirect-ne0-1 culled=1 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_CASE predication status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_BUNDLE bundle-indirect culled=0 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_BUNDLE bundle-indirect-predicated-culled culled=1 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_BUNDLE bundle-indirect-predicated-run culled=0 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_BUNDLE bundle-direct-predicated-culled culled=1 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_BUNDLE bundle-direct-predicated-run culled=0 status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE_CASE bundle status=PASS
OWNED_D3D12_RAYTRACING_COVERAGE failures=0
OWNED_D3D12_RAYTRACING_CASE hitobject status=PASS
"""

# Controlled CPU parser stimuli for the new coherence gate, NOT GPU evidence.
# Keep the arithmetic independent of runner constants/functions and mutate the
# serialized control records below, rather than inspect source wiring.
coherence_control_rows = []
for coherence_lane in range(64):
    seed = 0x43210000 + coherence_lane * 0x105
    before = seed ^ 0x13579bdf
    second = (seed * 7 + 0x10203040) & 0xffffffff
    tag = 0x73 if coherence_lane & 1 else 0x35
    payload = (((seed ^ 0x00ff00ff) + (second ^ 0x55aa55aa) + (before + tag) * 3) & 0xffffffff) ^ 0x2468ace0
    post = (payload + 0x6a09e667) & 0xffffffff
    values = (seed, post, before, before, payload, second, post ^ 0xbb67ae85, 2 if coherence_lane & 1 else 1)
    coherence_control_rows.append(
        f"OWNED_D3D12_RAYTRACING_HITOBJECT_COHERENCE_LANE lane={coherence_lane} words=("
        + ",".join(f"0x{word:08x}" for word in values) + ")\n")
RAYTRACING_COHERENCE_CONTROL_TRACE = ("".join(coherence_control_rows)
    + "OWNED_D3D12_RAYTRACING_HITOBJECT_COHERENCE lanes=64 words=512 scope=reorder wave_max=32 status=PASS\n"
    + "OWNED_D3D12_RAYTRACING_CASE hitobject-reorder-coherence status=PASS\n")
RAYTRACING_CONTRACT_TRACE += RAYTRACING_COHERENCE_CONTROL_TRACE


class RunnerFailures(unittest.TestCase):
    def test_multiqueue_waits_do_not_create_native_submissions(self):
        events = [{"event": kind} for kind, count in
                  (("execute_list", 64), ("signal_committed", 64), ("wait", 63), ("submit", 128), ("complete", 128))
                  for _ in range(count)]
        self.assertEqual(runner.multiqueue_submission_failures(events), [])
        self.assertTrue(runner.multiqueue_submission_failures(events + [{"event": "submit"}]))
        self.assertTrue(runner.multiqueue_submission_failures(events[:-1]))
        next(event for event in events if event["event"] == "wait")["submission"] = 99
        self.assertTrue(runner.multiqueue_submission_failures(events))

    def test_automatic_pipeline_cache_scopes_same_executable_logs_and_producers(self):
        # Synthetic trace protocol only: NOT compilation, GPU or disk persistence evidence.
        prefix = "OWNED_D3D12_PIPELINE_CACHE"
        keys = {name: digit * 40 for name, digit in zip(("a", "b", "root", "concurrent", "control"), "12345")}
        rows = [f"{prefix}_CASE_PASS {name}" for name in
                ("ordinary_key_memory", "disk_restore_invalid_rebuild", "concurrent_single_producer",
                 "failure_completion_retry", "driver_control_app_independence")]
        rows += [f"{prefix}_CHILD_PASS mode={name}" for name in ("disk", "corrupt", "truncated", "identity", "native")]
        rows += [f"{prefix}_NATIVE_FAULT field=metallib_version_major before=2 after=65535"]
        rows += [f"{prefix}_KEY case={name} key={key}" for name, key in keys.items()]
        rows += [f"{prefix}_KEY case=control_rebuilt key={keys['control']}"]
        rows += [f"{prefix}_KEY case=child key={keys['a']}" for _ in range(5)]
        labels = ("ordinary_a", "ordinary_root", "memory_a", "memory_root", "child_disk", "child_corrupt",
                  "child_truncated", "child_identity", "child_native", "failure_valid_control", "disabled_first",
                  "disabled_second", "enabled", "clear_rebuild", "unaligned_input_lifetime", "returned_blob_isolation")
        rows += [f"{prefix}_GPU case={label} actual=31415a5b expected=31415a5b"
                 for label in (*labels, *(f"concurrent_{i}" for i in range(8)))]
        rows += [f"{prefix}_GPU case={label} actual=31415813 expected=31415813"
                 for label in ("ordinary_b", "memory_b", "changed_shader_control")]
        rows += [f"{prefix}_REJECT case=pre_p3_version hr=887e0002"]
        rows += [f"{prefix}_REJECT case={label} hr=80070057" for label in
                 ("null_nonempty", "truncated", "corrupt_payload", "foreign_blob", "root_mismatch", "shader_mismatch")]
        stdout = "\n".join([*rows, prefix + "_PASS"]) + "\n"
        phases = {"parent": [], **{f"pipeline-cache-{name}": [] for name in
                  ("disk", "corrupt", "truncated", "identity", "native", "failure", "control")}}
        for name in ("a", "b", "root", "concurrent"):
            phases["parent"] += [{"event": "pipeline_cache_auto_begin", "key": keys[name]},
                                 {"event": "pipeline_cache_auto_publish", "key": keys[name], "hr": 0, "bytes": 1024},
                                 {"event": "pipeline_cache_auto_hit", "key": keys[name], "source": "memory"}]
        # Many helper PSO compiles and no waiter event must not invalidate a main-key single producer.
        phases["parent"] += [{"event": "pipeline_cache_cold", "key": "9" * 40}] * 17
        phases["pipeline-cache-disk"] = [{"event": "pipeline_cache_auto_hit", "key": keys["a"], "source": "disk"}]
        phases["pipeline-cache-native"] = [
            {"event": "pipeline_cache_auto_hit", "key": keys["a"], "source": "disk"},
            {"event": "pipeline_cache_auto_retry", "key": keys["a"]},
            {"event": "pipeline_cache_auto_publish", "key": keys["a"], "hr": 0, "bytes": 1024}]
        for phase in ("corrupt", "truncated", "identity"):
            phases[f"pipeline-cache-{phase}"] = [
                {"event": "pipeline_cache_auto_begin", "key": keys["a"]},
                {"event": "pipeline_cache_auto_publish", "key": keys["a"], "hr": 0, "bytes": 1024}]
        phases["pipeline-cache-failure"] = [
            {"event": "pipeline_cache_auto_begin", "key": "6" * 40},
            {"event": "pipeline_cache_auto_failure", "key": "6" * 40, "hr": 0x80070057, "bytes": 0}] * 2
        phases["pipeline-cache-control"] = [
            {"event": "pipeline_cache_auto_control", "flags": flags, "hr": 0} for flags in (6, 1, 2, 4)]
        phases["pipeline-cache-control"] += [
            {"event": "pipeline_cache_auto_begin", "key": keys["control"]},
            {"event": "pipeline_cache_auto_publish", "key": keys["control"], "hr": 0, "bytes": 1024},
            {"event": "pipeline_cache_auto_bypass", "key": keys["control"], "reason": "disabled"}] * 2
        phases["pipeline-cache-control"] += [{"event": "pipeline_cache_auto_hit", "key": keys["control"], "source": "memory"}]
        logs = {("same_d3d12.log" if phase == "parent" else phase + "/same_d3d12.log"):
                "\n".join("OWNED_GRAPHICS_TRACE " + json.dumps(event) for event in events)
                for phase, events in phases.items()}
        self.assertEqual(runner.pipeline_cache_failures(stdout, "", logs), [])
        reordered_controls = [event for event in phases["pipeline-cache-control"]
                              if event["event"] != "pipeline_cache_auto_control"]
        reordered_controls += [{"event": "pipeline_cache_auto_control", "flags": flags, "hr": 0}
                               for flags in (4, 2, 1, 2)]
        reordered_logs = logs | {"pipeline-cache-control/same_d3d12.log":
            "\n".join("OWNED_GRAPHICS_TRACE " + json.dumps(event) for event in reordered_controls)}
        self.assertEqual(runner.pipeline_cache_failures(stdout, "", reordered_logs), [])
        for label, corrupted in (
                ("second producer", logs | {"same_d3d12.log": logs["same_d3d12.log"] + "\nOWNED_GRAPHICS_TRACE " +
                    json.dumps({"event": "pipeline_cache_auto_begin", "key": keys["concurrent"]})}),
                ("missing disk log", {name: text for name, text in logs.items() if not name.startswith("pipeline-cache-disk/")}),
                ("native retry missing", logs | {"pipeline-cache-native/same_d3d12.log":
                    logs["pipeline-cache-native/same_d3d12.log"].replace("pipeline_cache_auto_retry", "unrelated_event")}),
                ("merged child log", {name.split("/")[-1]: text for name, text in logs.items()}),
                ("failed control", logs | {"pipeline-cache-control/same_d3d12.log":
                    logs["pipeline-cache-control/same_d3d12.log"] + "\nOWNED_GRAPHICS_TRACE " +
                    json.dumps({"event": "pipeline_cache_auto_control", "flags": 4, "hr": 0x80070005})}),
                ("disable not covered", logs | {"pipeline-cache-control/same_d3d12.log":
                    logs["pipeline-cache-control/same_d3d12.log"].replace('"flags": 1', '"flags": 2')}),
                ("disabled cache hit", logs | {"pipeline-cache-control/same_d3d12.log":
                    logs["pipeline-cache-control/same_d3d12.log"].replace("pipeline_cache_auto_bypass", "pipeline_cache_auto_hit")})):
            with self.subTest(label=label):
                self.assertTrue(runner.pipeline_cache_failures(stdout, "", corrupted))

    def test_native_core_loads_require_owned_paths_bytes_and_guest_abi(self):
        # Controlled PE headers and Wine trace protocol, not real Core execution.
        with tempfile.TemporaryDirectory(dir=SCRATCH, prefix="native-core-") as directory:
            base = Path(directory)
            runtime, frozen = base / "runtime", base / "fixture"
            generation = "0123456789abcdef0123456789abcdef"
            modules = {}
            for abi, machine, magic, subdir in (("x64", 0x8664, 0x20B, "x86_64"), ("x86", 0x14C, 0x10B, "i386")):
                image = bytearray(controlled_pe_fixture())
                struct.pack_into("<H", image, 0x44, machine)
                struct.pack_into("<H", image, 0x58, magic)
                source = runtime / f"lib/wine/{subdir}-windows/d3d12core.dll"
                source.parent.mkdir(parents=True)
                source.write_bytes(image + generation.encode())
                modules[str(source.relative_to(runtime))] = {"sha256": runner.digest(source), "generation": generation}
            manifest = {"agility_sdk": {"delivery": "native-only", "modules": modules}}
            for abi, subdir in (("x64", "x86_64"), ("x86", "i386")):
                with self.subTest(abi=abi):
                    arch = {"guest_arch": abi, "windows_abi": "x64", "windows_directory": "lib/wine/x86_64-windows"}
                    source = runtime / f"lib/wine/{subdir}-windows/d3d12core.dll"
                    for name in ("sdk-owned", "sdk-alternate", "sdk-other-generation"):
                        copy = frozen / name / "d3d12core.dll"
                        copy.parent.mkdir(parents=True, exist_ok=True)
                        copy.write_bytes(source.read_bytes())
                    def trace(path, mode="native"):
                        windows = ("Z:" + str(path).replace("/", "\\")).replace("\\", "\\\\")
                        return f'0024:trace:loaddll:build_module Loaded L"{windows}" at 00006FFFFF460000: {mode}\n'
                    selected = frozen / "sdk-owned/d3d12core.dll"
                    alternate = frozen / "sdk-alternate/d3d12core.dll"
                    evidence = runner.owned_core_load_evidence(trace(source) + trace(selected) + trace(alternate), runtime, manifest, arch, frozen, {}, require_load=True)
                    self.assertEqual(evidence["failures"], [])
                    self.assertEqual(evidence["foreign_loads"], [])
                    self.assertEqual([row["abi"] for row in evidence["loads"]], [abi] * 3)
                    for omitted in (source, selected, alternate):
                        incomplete = "".join(trace(path) for path in (source, selected, alternate) if path != omitted)
                        self.assertTrue(runner.owned_core_load_evidence(incomplete, runtime, manifest, arch, frozen, {}, require_load=True)["failures"])
                    ordinary = runner.owned_core_load_evidence(trace(source), runtime, manifest, arch, frozen, require_load=True)
                    self.assertEqual(ordinary["failures"], [])
                    self.assertEqual(len(ordinary["loads"]), 1)
                    original = source.read_bytes()
                    source.write_bytes(original + b"changed-installed-image")
                    changed = runner.owned_core_load_evidence(trace(source), runtime, manifest, arch, frozen, require_load=True)
                    self.assertTrue(changed["failures"])
                    self.assertEqual(len(changed["foreign_loads"]), 1)
                    source.write_bytes(original)
                    for rejected in (trace(frozen / "sdk-other-generation/d3d12core.dll"), trace(selected, "builtin")):
                        result = runner.owned_core_load_evidence(rejected, runtime, manifest, arch, frozen, {}, require_load=True)
                        self.assertEqual(len(result["foreign_loads"]), 1)
                    selected.write_bytes(selected.read_bytes() + b"different-generation")
                    result = runner.owned_core_load_evidence(trace(selected), runtime, manifest, arch, frozen, {}, require_load=True)
                    self.assertTrue(result["failures"])
                    self.assertEqual(len(result["foreign_loads"]), 1)
                    self.assertTrue(runner.owned_core_load_evidence("", runtime, manifest, arch, frozen, require_load=True)["failures"])
                    self.assertEqual(runner.owned_core_load_evidence("", runtime, manifest, arch, frozen)["failures"], [])
            # A matching hash cannot turn an opposite-ABI image into admission.
            wrong = runtime / "lib/wine/x86_64-windows/d3d12core.dll"
            wrong.write_bytes((runtime / "lib/wine/i386-windows/d3d12core.dll").read_bytes())
            modules[str(wrong.relative_to(runtime))]["sha256"] = runner.digest(wrong)
            arch = {"guest_arch": "x64", "windows_abi": "x64", "windows_directory": "lib/wine/x86_64-windows"}
            self.assertTrue(runner.owned_core_load_evidence(trace(wrong), runtime, manifest, arch, frozen)["failures"])
            retained = runner.owned_core_load_evidence(trace(wrong, "builtin"), runtime, {}, arch, frozen)
            self.assertEqual(retained["foreign_loads"], [])
            self.assertEqual(len(runner.owned_core_load_evidence(trace(wrong), runtime, {}, arch, frozen)["foreign_loads"]), 1)

    def simulate_metalfx(self, suite: str, stdout: str, *, builtin: bool = True,
                         native: bool = True, selected: bool = True, validated: bool = True) -> dict:
        with tempfile.TemporaryDirectory(dir=SCRATCH, prefix="metalfx-") as directory:
            base = Path(directory)
            runtime = base / "runtime"
            (runtime / "lib/wine/x86_64-unix").mkdir(parents=True)
            (runtime / "lib/wine/x86_64-windows").mkdir()
            if native:
                (runtime / "lib/wine/x86_64-unix/winemetal.so").write_bytes(b"native")
            module = ("amd_fidelityfx_upscaler_dx12.dll" if suite in ("fsr", "fsr-debug")
                      else "dcomp.dll" if suite == "composition" else "amd_fidelityfx_framegeneration_dx12.dll")
            (runtime / "lib/wine/x86_64-windows" / module).write_bytes(b"owned fixture module")
            manifest = {"schema": 1, "optional_modules": {"fsr": selected, "fsr_framegeneration": selected, "dcomp": selected}}
            if suite == "composition":
                image = runtime / "lib/wine/x86_64-windows/dcomp.dll"
                image.write_bytes(controlled_pe_fixture())
                manifest["wine_display_frontend"] = {"source_pin": "913e31f201d344223bdf3d13a50a41af35893d12",
                    "modules": {"lib/wine/x86_64-windows/dcomp.dll": {"sha256": runner.digest(image)}}}
            (runtime / "owned-graphics-runtime.json").write_text(json.dumps(manifest))
            fixture = base / f"owned-d3d12-{suite}.exe"
            fixture.write_bytes(controlled_pe_fixture())
            args = argparse.Namespace(runtime=runtime, lock=runtime / "owned-graphics-runtime.json",
                                      fixture=fixture, dxil_dir=base, out=base / "result", suite=suite,
                                      mode="no-present", validation=False, api_validation=suite == "composition" and validated, timeout=1)

            def fake_run(command, **kwargs):
                if command[0] == str(runtime / "bin/wine"):
                    kwargs["stdout"].write(stdout)
                    kwargs["stderr"].write(f"dyld[1]: {runtime}/lib/wine/x86_64-unix/winemetal.so\n")
                    kwargs["stderr"].write(r'Loaded L"C:\\windows\\system32\\' + module +
                                           f'" at 0001: {"builtin" if builtin else "native"}\n')
                    self.assertNotIn("OWNED_FG_OBSERVER", kwargs["env"])
                    self.assertNotIn("FG_CAPTURE_DIR", kwargs["env"])
                return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

            with patch.object(runner.subprocess, "run", side_effect=fake_run), \
                 patch.dict(runner.os.environ, {"OWNED_FG_OBSERVER": "/foreign/observer.dylib",
                                               "FG_CAPTURE_DIR": "/foreign/captures"}), \
                 contextlib.redirect_stdout(io.StringIO()):
                runner.run(args)
            return json.loads((args.out / "result.json").read_text())

    def test_diagnostics_requires_factory_gpu_isolation_and_all_contracts(self):
        # Synthetic parser regression only, not GPU evidence.
        prefix = "OWNED_D3D12_DIAGNOSTICS"
        names = ("agility_installed_factory_gpu_isolation", "validation", "storage_retrieval",
                 "filters_callbacks", "native_markers_dred_removal", "sdk_tools_contracts")
        trace = prefix + "_BACKEND id=owned-graphics-d3d12\n"
        trace += "".join(prefix + "_CASE_PASS case=" + name + "\n" for name in names)
        trace += prefix + "_PASS cases=6\n"
        self.assertEqual(runner.diagnostics_failures(trace, ""), [])
        for name in names:
            line = prefix + "_CASE_PASS case=" + name + "\n"
            self.assertTrue(runner.diagnostics_failures(trace.replace(line, ""), ""))
            self.assertTrue(runner.diagnostics_failures(trace + line, ""))
        self.assertTrue(runner.diagnostics_failures(trace, prefix + "_FAIL step=gpu"))
        self.assertFalse(self.simulate_metalfx("diagnostics", trace, native=False)["pass"])
        self.assertTrue(self.simulate_metalfx("diagnostics", trace)["pass"])

    def test_agility_parser_requires_real_selection_gpu_lifetime_unload_and_child_contracts(self):
        # Controlled protocol input only, never GPU or observed module-load evidence.
        prefix = "OWNED_D3D12_AGILITY"
        generation = "0123456789abcdef0123456789abcdef"
        path = Path("/evidence/fixture/sdk-owned/d3d12core.dll")
        wine_path = "Z:" + str(path).replace("/", "\\")
        installed = Path("/evidence/runtime/lib/wine/x86_64-windows/d3d12core.dll")
        inbox_path = "Z:" + str(installed).replace("/", "\\")
        names = ("rejected_modules", "selected_core_path_version", "factory_configuration_gpu",
                 "retained_objects_gpu", "unused_core_unload", "set_sdk_version_fresh_process")
        lines = [prefix + "_CASE_PASS case=" + name for name in names]
        lines += [prefix + "_REJECT case=" + name + " output=null loaded=0 hr=0x80070057" for name in
                  ("wrong_version", "missing_path", "missing_file", "foreign_module", "wrong_abi", "different_generation")]
        lines += [prefix + f"_GPU phase={phase} expected={word} actual={word} sdk=618" for phase, word in
                  (("configured", "a6180001"), ("retained", "a6180002"), ("children", "a6180003"),
                   ("set_sdk_lower_child", "a6180009"), ("set_sdk_before_late", "a6180004"),
                   ("set_sdk_child", "a6180005"), ("apply_global_child", "a6180006"),
                   ("initialized_alternate_child", "a6180007"), ("executable_lower_child", "a618000a"), ("executable_exports_child", "a6180008"))]
        lines += [prefix + f"_INBOX_SELECTION operation={operation} requested={requested} exe_sdk={exe_sdk} sdk=618 generation={generation} path={inbox_path} app_loaded=0 cwd_base=exe"
                  for operation, requested, exe_sdk in (("set_sdk", 617, 619), ("set_sdk", 618, 619),
                                                       ("exe_exports", 617, 617), ("exe_exports", 618, 618))]
        lines += [prefix + f"_CORE sdk=618 generation={generation} path={wine_path} cwd_base=exe source=staged-copy",
                  prefix + f"_UNLOAD path={wine_path} live_objects=0 loaded=0",
                  prefix + "_DIRECT_CORE_FACTORY sdk=618 selected_module=retained owners=1 final_unload=1",
                  prefix + "_CONCURRENT_FREE rounds=32 factories=32 configurations=32 sdk=618 cold_start=1 module=stable final_unload=1",
                  prefix + "_EXECUTABLE_REJECT requested=619 output=null app_loaded=0 hr=0x80070057",
                  prefix + "_SET_SDK_REJECT requested=619 selection=UNCHANGED app_loaded=0 hr=0x80070057",
                  prefix + "_SET_SDK_REJECT requested=619 selection=UNCHANGED app_loaded=0 hr=0x80070057",
                  prefix + "_DEVELOPER mode=set_sdk_lower ordinary_set=DENIED ordinary_sdk1=AVAILABLE source=os-registry",
                  prefix + "_DEVELOPER mode=set_sdk ordinary_set=DENIED ordinary_sdk1=AVAILABLE source=os-registry",
                  prefix + "_DEVELOPER mode=apply_global ordinary_set=DENIED ordinary_sdk1=AVAILABLE source=os-registry",
                  prefix + "_HRESULT operation=ordinary_set hr=0x80004005",
                  prefix + "_HRESULT operation=ordinary_set hr=0x80004005",
                  prefix + "_HRESULT operation=late_set hr=0x80004005",
                  prefix + "_HRESULT operation=late_global_removal hr=0x80004005",
                  prefix + "_HRESULT operation=ordinary_set hr=0x80004005",
                  prefix + "_HRESULT operation=alternate_apply hr=0x887e0003",
                  prefix + "_SET_SDK before_device=PASS late_call=FAIL existing_global=REMOVED independent_device=LIVE selection=UNCHANGED os_state=RESTORED sdk=618",
                  prefix + "_SET_SDK_LOWER before_device=PASS exe_sdk=619 os_state=RESTORED sdk=618",
                  prefix + "_APPLY_GLOBAL ordinary_mode=PASS sdk=618 selected_core=RETAINED initialized_factory=PASS alternate_apply=REJECTED global_device=LIVE independent_device=LIVE os_state=RESTORED",
                  prefix + "_PASS cases=6"]
        trace = "\n".join(lines) + "\n"
        self.assertEqual(runner.agility_failures(trace, "", path, generation, installed), [])
        for line in lines:
            self.assertTrue(runner.agility_failures(trace.replace(line + "\n", ""), "", path, generation, installed), line)
            self.assertTrue(runner.agility_failures(trace + line + "\n", "", path, generation, installed), line)
        self.assertEqual(runner.agility_failures(trace.replace("hr=0x80004005", "hr=0x887a0001").replace("hr=0x80070057", "hr=0x80004001"), "", path, generation, installed), [])
        for old, new in (("actual=a6180003", "actual=00000000"), ("output=null", "output=stale"),
                         ("hr=0x80004005", "hr=0x00000000"), ("hr=0x80070057", "hr=0x00000001"),
                         ("hr=0x887e0003", "hr=0x80004005"),
                         ("loaded=0", "loaded=1"), ("sdk=618", "sdk=619"), ("cwd_base=exe", "cwd_base=cwd"),
                         ("exe_sdk=619", "exe_sdk=618"), ("selection=UNCHANGED", "selection=CHANGED"),
                         (inbox_path, wine_path), ("requested=617", "requested=616"),
                         ("actual=a6180009", "actual=00000000"), ("actual=a618000a", "actual=00000000")):
            self.assertTrue(runner.agility_failures(trace.replace(old, new), "", path, generation, installed))
        self.assertTrue(runner.agility_failures(trace, "", Path("/foreign/d3d12core.dll"), generation, installed))
        self.assertTrue(runner.agility_failures(trace, "", path, generation, Path("/foreign/d3d12core.dll")))
        self.assertTrue(runner.agility_failures(trace, "", path, "f" * 32, installed))
        self.assertTrue(runner.agility_failures(trace, prefix + "_FAIL step=copy", path, generation, installed))

    def test_corewindow_reachable_contracts_never_claim_feature_completion(self):
        # Synthetic CPU-only protocol input, not observed CoreWindow/GPU evidence.
        trace = """OWNED_D3D12_COREWINDOW_COMPOSITION_BACKEND id=owned-graphics-d3d12
OWNED_D3D12_COREWINDOW_COMPOSITION_CONTRACT step=null-corewindow hr=0x887a0001
OWNED_D3D12_COREWINDOW_COMPOSITION_CONTRACT step=non-corewindow-object hr=0x887a0001
OWNED_D3D12_COREWINDOW_COMPOSITION_CONTRACT step=hwnd-no-corewindow hr=0x887a0001
OWNED_D3D12_COREWINDOW_COMPOSITION_CONTRACT step=hwnd-no-matrix hr=0x887a0001
OWNED_D3D12_COREWINDOW_COMPOSITION_CONTRACT step=hwnd-no-matrix-get hr=0x887a0001
OWNED_D3D12_COREWINDOW_COMPOSITION_CONTRACT step=hdr10plus hr=0x887a0004
OWNED_D3D12_COREWINDOW_COMPOSITION_CAPS hardware=0 overlay=0 overlays=0 stereo=0
OWNED_D3D12_COREWINDOW_COMPOSITION_CONTRACT step=stereo-swapchain hr=0x887a0004
OWNED_D3D12_COREWINDOW_COMPOSITION_CONTRACT step=actual-corewindow-runtime hr=0x80040154
OWNED_D3D12_COREWINDOW_COMPOSITION_COREWINDOW actual=0 gpu_present=0
OWNED_D3D12_COREWINDOW_COMPOSITION_PREREQUISITE corewindow=coreapplication-ui-thread-window-required
OWNED_D3D12_COREWINDOW_COMPOSITION_REACHABLE_OK corewindow=separate-prerequisite
"""
        prefix = "OWNED_D3D12_COREWINDOW_COMPOSITION"
        # Native desktop GetForCurrentThread can succeed with NULL. That does
        # not prove a runtime defect or fulfill a CoreApplication UI-thread test.
        desktop = trace.replace("step=actual-corewindow-runtime hr=0x80040154",
                                "step=actual-corewindow-runtime hr=0x00000000")
        desktop_evidence = runner.corewindow_composition_evidence(desktop)
        self.assertEqual(desktop_evidence["failures"], [])
        self.assertFalse(desktop_evidence["feature_complete"])
        self.assertEqual(desktop_evidence["prerequisites"]["corewindow"], "coreapplication-ui-thread-window-required")
        for actual in (0, 1):
            candidate = trace
            if actual:
                candidate = candidate.replace("actual=0 gpu_present=0", "actual=1 gpu_present=1")
                candidate = candidate.replace("step=actual-corewindow-runtime hr=0x80040154",
                                              "step=actual-corewindow-runtime hr=0x00000000")
                candidate = candidate.replace(prefix + "_PREREQUISITE corewindow=coreapplication-ui-thread-window-required\n", "")
            report = self.simulate_metalfx("corewindow-composition", candidate)
            self.assertFalse(report["pass"])
            self.assertFalse(report["feature_complete"])
            self.assertEqual(report["result"], "REACHABLE_OK")
            self.assertTrue(report["reachable_only_verified"])
            evidence = report["corewindow_composition"]
            self.assertEqual(evidence["actual_corewindow"], actual)
            self.assertEqual(evidence["composition"], "separate-suite")
            self.assertEqual(evidence["matrix_pixel_oracle"], "separate-suite")
            self.assertEqual(evidence["failures"], [])
            self.assertEqual(self.simulate_metalfx("corewindow-composition", candidate, native=False)["result"], "FAIL")
        for line in trace.splitlines():
            with self.subTest(line=line):
                self.assertTrue(runner.corewindow_composition_evidence(trace.replace(line + "\n", ""))["failures"])
                self.assertTrue(runner.corewindow_composition_evidence(trace + line + "\n")["failures"])
        for broken in (trace.replace("REACHABLE_OK corewindow=separate-prerequisite", "PASS"),
                       trace + prefix + "_CONTRACT step=composition-swapchain hr=0x80004001\n",
                       trace.replace("actual=0 gpu_present=0", "actual=1 gpu_present=0"),
                       trace.replace("hardware=0", "hardware=1"),
                       trace.replace("step=hwnd-no-matrix hr=0x887a0001", "step=hwnd-no-matrix hr=0x00000000")):
            with self.subTest(broken=broken):
                self.assertEqual(self.simulate_metalfx("corewindow-composition", broken)["result"], "FAIL")
        self.assertTrue(runner.corewindow_composition_evidence(trace, prefix + "_FAIL step=present")["failures"])

    def test_composition_positive_contracts_do_not_manufacture_visual_or_corewindow_proof(self):
        # Controlled protocol input, NOT observed GPU/CoreAnimation evidence.
        trace = """OWNED_D3D12_COMPOSITION_BACKEND id=owned-graphics-d3d12
OWNED_D3D12_COMPOSITION_GPU case=premul size=80x64 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_GPU case=straight size=80x64 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_CASE_PASS case=godot_alpha_present
OWNED_D3D12_COMPOSITION_GPU case=matrix_premul size=80x64 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_GPU case=matrix_straight size=80x64 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_CASE_PASS case=matrix_next_present_validation
OWNED_D3D12_COMPOSITION_CASE_PASS case=no_hwnd_restrictions
OWNED_D3D12_COMPOSITION_GPU case=source_premul size=80x64 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_GPU case=source_straight size=80x64 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_GPU case=resize_premul size=96x80 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_GPU case=resize_straight size=96x80 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_CASE_PASS case=source_resize
OWNED_D3D12_COMPOSITION_CASE_PASS case=commit_detach_ownership
OWNED_D3D12_COMPOSITION_QUEUE_ORDER phase=before_release completed_b=0 completed_a=0
OWNED_D3D12_COMPOSITION_QUEUE_ORDER phase=after_release completed_b=1 completed_a=1
OWNED_D3D12_COMPOSITION_GPU case=queued_a_premul size=96x80 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_GPU case=queued_b_premul size=96x80 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_GPU case=queued_a_straight size=96x80 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_GPU case=queued_b_straight size=96x80 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_GPU case=destroyed_window size=80x64 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_CASE_PASS case=queued_present_content_window_lifetime
OWNED_D3D12_COMPOSITION_GPU case=ordinary_hwnd size=80x64 samples=5 readback=exact
OWNED_D3D12_COMPOSITION_CASE_PASS case=ordinary_hwnd_regression
OWNED_D3D12_COMPOSITION_PASS cases=7 gpu=14 visual=not-observed corewindow=separate-prerequisite
"""
        report = self.simulate_metalfx("composition", trace)
        self.assertTrue(report["pass"])
        self.assertFalse(report["feature_complete"])
        self.assertEqual(report["composition"]["source_gpu_readbacks"], 14)
        self.assertEqual(report["composition"]["onscreen_alpha_matrix"], "not-observed")
        self.assertEqual(report["composition"]["corewindow"], "separate-prerequisite")
        for options in ({"native": False}, {"builtin": False}, {"selected": False}, {"validated": False}):
            with self.subTest(options=options):
                self.assertFalse(self.simulate_metalfx("composition", trace, **options)["pass"])
        for line in trace.splitlines():
            with self.subTest(line=line):
                self.assertTrue(runner.composition_evidence(trace.replace(line + "\n", ""))["failures"])
                self.assertTrue(runner.composition_evidence(trace + line + "\n")["failures"])
        for broken in (trace.replace("readback=exact", "readback=unverified", 1),
                       trace.replace("completed_b=0 completed_a=0", "completed_b=1 completed_a=0"),
                       trace.replace("completed_b=0 completed_a=0", "completed_b=0 completed_a=1"),
                       trace.replace("size=96x80", "size=80x64", 1),
                       trace.replace("corewindow=separate-prerequisite", "corewindow=complete"),
                       trace.replace("visual=not-observed", "visual=PASS")):
            self.assertTrue(runner.composition_evidence(broken)["failures"])
        self.assertTrue(runner.composition_evidence(trace, "OWNED_D3D12_COMPOSITION_FAIL step=present")["failures"])
        held = trace.replace("visual=not-observed", "visual=external-capture-required")
        phases = ("identity", "matrix-pending", "matrix-presented", "source-cropped", "resized",
                  "content-pending", "content-detached", "root-pending", "root-detached", "reattached",
                  "queued-before-release", "queued-captured-identity", "content-lifetime")
        records = "".join(f"OWNED_D3D12_COMPOSITION_VISUAL_READY phase={phase} left=0x1001 right=0x1002 control=Z:\\control.txt\n"
                          f"OWNED_D3D12_COMPOSITION_VISUAL_ADVANCED phase={phase}\n" for phase in phases)
        held += records
        evidence = runner.composition_evidence(held, visual=True)
        self.assertEqual(evidence["failures"], [])
        self.assertFalse(evidence["feature_complete"])
        self.assertEqual(evidence["onscreen_alpha_matrix"], "external-capture-required")
        self.assertTrue(runner.composition_evidence(held)["failures"])
        for line in records.splitlines():
            self.assertTrue(runner.composition_evidence(held.replace(line + "\n", ""), visual=True)["failures"])
            self.assertTrue(runner.composition_evidence(held + line + "\n", visual=True)["failures"])
        self.assertTrue(runner.composition_evidence(held.replace("right=0x1002", "right=0x1001"), visual=True)["failures"])
        self.assertTrue(runner.composition_evidence(held + "OWNED_D3D12_COMPOSITION_VISUAL_READY malformed\n", visual=True)["failures"])
        first_ready, first_advanced = records.splitlines()[:2]
        reordered = held.replace(first_ready + "\n" + first_advanced, first_advanced + "\n" + first_ready)
        self.assertTrue(runner.composition_evidence(reordered, visual=True)["failures"])

    def test_composition_module_audit_rejects_wrong_bytes_abi_or_load(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH, prefix="composition-module-") as directory:
            runtime = Path(directory)
            arch = {"guest_arch": "x64", "windows_abi": "x64", "windows_directory": "lib/wine/x86_64-windows"}
            relative = "lib/wine/x86_64-windows/dcomp.dll"
            image = runtime / relative
            image.parent.mkdir(parents=True)
            image.write_bytes(controlled_pe_fixture())
            manifest = {"optional_modules": {"dcomp": True}, "wine_display_frontend": {
                "source_pin": "913e31f201d344223bdf3d13a50a41af35893d12", "modules": {relative: {"sha256": runner.digest(image)}}}}
            trace = r'Loaded L"C:\\windows\\system32\\dcomp.dll" at 0001: builtin'
            self.assertEqual(runner.composition_module_evidence(trace, runtime, manifest, arch)["failures"], [])
            for missing in ("", trace.replace("builtin", "native"), trace.replace("C:", "D:")):
                self.assertTrue(runner.composition_module_evidence(missing, runtime, manifest, arch)["failures"])
            original = image.read_bytes()
            image.write_bytes(original + b"changed")
            self.assertTrue(runner.composition_module_evidence(trace, runtime, manifest, arch)["failures"])
            opposite = bytearray(original)
            struct.pack_into("<H", opposite, 0x44, 0x14C)
            struct.pack_into("<H", opposite, 0x58, 0x10B)
            image.write_bytes(opposite)
            manifest["wine_display_frontend"]["modules"][relative]["sha256"] = runner.digest(image)
            self.assertTrue(runner.composition_module_evidence(trace, runtime, manifest, arch)["failures"])

    def test_fsr_fg_summary_without_gpu_contracts_cannot_pass(self):
        report = self.simulate_metalfx("fsr-fg",
            "OWNED_D3D12_FSR_FG_BACKEND id=owned-graphics-d3d12\nOWNED_D3D12_FSR_FG_PASS\n")
        self.assertFalse(report["pass"])

    def test_fsr_fg_legacy_sr_only_runtime_is_not_relabelled_fg(self):
        with self.assertRaisesRegex(ValueError, "fsr_framegeneration=true"):
            self.simulate_metalfx("fsr-fg", "", selected=False)

    def test_fsr_fg_contracts_require_builtin_and_native_module_evidence(self):
        # Controlled output from the independently specified twelve-frame scenario,
        # not generated from runner constants. Native present proof is separate.
        trace = """OWNED_D3D12_FSR_FG_BACKEND id=owned-graphics-d3d12
OWNED_D3D12_FSR_FG_DIRECT_PASS snapshot=exact rect_guard=exact frames=4
OWNED_D3D12_FSR_FG_HISTORY_PASS reset=fresh gap=reset off_on=fresh stale_record=no_history_publication negative_output=unchanged
OWNED_D3D12_FSR_FG_LIFETIME_PASS context_destroy_before_execute=1 input_release=1 allocation_balance=1
OWNED_D3D12_FSR_FG_CONFIG_PASS repeated_id=replace retained_distinct_limit=64 overflow=runtime_error off_clears=1
OWNED_D3D12_FSR_FG_DEBUG_PASS tear=gpu_effect reset=history debug=real_inputs configure=frame_snapshot history=unmodified alpha=opaque
OWNED_D3D12_FSR_FG_DEBUG_CAPTURE_PASS prepare=per_frame configure=recorded rect=recorded output=independent_gpu_exact
OWNED_D3D12_FSR_FG_DEBUG_REPLAY_PASS combined=1 debug_overlay=full_display replay=exact context_destroy_before_replay=1
OWNED_D3D12_FSR_FG_COUNT_REJECTION_PASS counts=0,5 sentinel=exact prepare=retained multi_output_supported=1
OWNED_D3D12_FSR_FG_NO_NOTIFY_PROXY_PASS frame=3 generated=2 result_observer=retained
OWNED_D3D12_FSR_FG_PREPARE_ORDER_PASS matching_frame=2 newest_prepare=3 output=exact
OWNED_D3D12_FSR_FG_GROUPED_PASS count=1 distinct_gpu_outputs=1 rect_guard=exact reset_output=unchanged
OWNED_D3D12_FSR_FG_GROUPED_PASS count=2 distinct_gpu_outputs=2 rect_guard=exact reset_output=unchanged
OWNED_D3D12_FSR_FG_GROUPED_PASS count=3 distinct_gpu_outputs=3 rect_guard=exact reset_output=unchanged
OWNED_D3D12_FSR_FG_GROUPED_PASS count=4 distinct_gpu_outputs=4 rect_guard=exact reset_output=unchanged
OWNED_D3D12_FSR_FG_MULTI_PACING_PASS counts=1,2,3,4 distinct_gpu_outputs=1 generated_before_real=1 bootstrap_reset_scenechange_disabled=zero only_generated_zero=no_present
OWNED_D3D12_FSR_FG_QUEUES_PASS samequeue=1 nullarray=rejected count0=preserved multipqueue=3 device_type_mask=rejected rollback=atomic drain=oldframe owners=retained generated_real_ui=selected worker_idle=progress
OWNED_D3D12_FSR_FG_RECORDING_ROLLBACK frame=2 preserved=1 consumed_by=public_present_gpu_output
OWNED_D3D12_FSR_FG_REJECTED_GENERATION_ORDER frame=9 real_gpu_output=after_current_game_batch generated=absent
OWNED_D3D12_FSR_FG_PROXY_PASS create=hwnd callbacks=gpu-readback generated_before_real=1 only_generated=1 disabled=1 callback_error=1 drain=1 destroy=1
OWNED_D3D12_FSR_FG_PACING_PASS ordered=1 bounded_progress=1 qpc=observed native_timestamps=external_observer_required
OWNED_D3D12_FSR_FG_UI_PASS wrap=real_hwnd ui=builtin_shader premultiplied=1 doublebuffer=gpu_snapshot resize=96x80 lifetime=drained
OWNED_D3D12_FSR_FG_UI_FRAME frame=11 width=128 height=128 source=88,60,100 ui=64,0,0,128 premul=1 doublebuffer=1 overwritten_after_present=1
OWNED_D3D12_FSR_FG_UI_FRAME frame=12 width=128 height=128 source=96,60,100 ui=64,0,0,128 premul=1 doublebuffer=1 overwritten_after_present=1
OWNED_D3D12_FSR_FG_UI_FRAME frame=13 width=96 height=80 source=104,60,100 ui=64,0,0,128 premul=1 doublebuffer=1 overwritten_after_present=1
OWNED_D3D12_FSR_FG_UI_FRAME frame=14 width=96 height=80 source=112,60,100 ui=64,0,0,128 premul=1 doublebuffer=1 overwritten_after_present=1
OWNED_D3D12_FSR_FG_PASS
"""
        for frame, enabled, only, gen_error, present_error in (
                (1, 1, 0, 0, 0), (2, 1, 0, 0, 0), (3, 1, 0, 0, 0), (4, 1, 0, 0, 0),
                (5, 0, 0, 0, 0), (6, 0, 0, 0, 0), (7, 1, 0, 0, 0), (8, 1, 1, 0, 0),
                (9, 1, 0, 1, 0), (10, 1, 0, 0, 1), (11, 1, 0, 0, 0), (12, 1, 1, 0, 0)):
            trace += (f"OWNED_D3D12_FSR_FG_PRESENT_CALL frame={frame} enabled={enabled} only_generated={only} "
                      f"generation_error={gen_error} present_error={present_error} "
                      f"qpc_start={frame * 100} qpc_end={frame * 100 + 10} frequency=1000\n")
        for frame, generated, index in ((1, 0, 0), (2, 1, 1), (2, 0, 0),
                (3, 1, 1), (3, 1, 2), (3, 0, 0), (4, 1, 1), (4, 1, 2), (4, 1, 3), (4, 0, 0),
                (5, 0, 0), (6, 0, 0), (7, 0, 0), (8, 1, 1), (8, 1, 2), (8, 1, 3), (8, 1, 4), (9, 0, 0), (11, 0, 0)):
            trace += (f"OWNED_D3D12_FSR_FG_CALLBACK_PIXELS frame={frame} generated={generated} "
                      f"output_index={index} width=128 height=128 hash={frame * 16 + index:016x} marker={frame + 32},{190 if generated else 70},220\n")
        for stream, frames in (("callback", [*range(1, 10), 11, 12]), ("builtin_ui", list(range(11, 15)))):
            for frame in frames:
                submitted = (sum(1 + {2: 1, 3: 2, 4: 3, 8: 4}.get(f, 0) - int(f in (8, 10, 12))
                                 for f in range(1, frame + 1)) if stream == "callback" else frame - 10)
                unmeasured = frame == frames[0]
                measured = 0 if unmeasured else max(1, submitted - 1)
                refresh = 0 if unmeasured else submitted
                trace += (f"OWNED_D3D12_FSR_FG_STATISTICS frame={frame} statistics={'disjoint' if unmeasured else 'actual'} "
                          f"result={'0x887a000b' if unmeasured else '0x00000000'} submitted_present={submitted} native_present={measured} "
                          f"present_refresh={refresh} sync_refresh={refresh} sync_qpc={measured * 100} qpc_end=20000 frequency=1000 source=public_dxgi_actual_presentation\n")
            trace += (f"OWNED_D3D12_FSR_FG_FINAL_STATISTICS stream={stream} output_vblanks=3 statistics=actual "
                      f"result=0x00000000 submitted_present={submitted} native_present={measured} present_refresh={refresh} "
                      f"sync_refresh={refresh} sync_qpc={measured * 100} qpc_end=20000 frequency=1000 source=public_dxgi_actual_presentation\n")
        for frame, composition in ((1, "pending_marker"), (3, "pending_marker"), (5, "pending_marker"),
                                   (9, "pending_marker"), (11, "builtin_ui"), (12, "builtin_ui")):
            trace += (f"OWNED_D3D12_FSR_FG_QUEUE_PROOF frame={frame} blocked=selected idle=progress "
                      f"composition={composition} submitted_before={frame * 2} submitted_during={frame * 2 + int(frame == 5)} "
                      f"counter_domain=public_present_calls\n")
        self.assertTrue(self.simulate_metalfx("fsr-fg", trace)["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("result=0x887a000b", "result=0x00000000", 1))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("qpc_end=20000", "qpc_end=1"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("output_vblanks=3", "output_vblanks=0"))["pass"])
        self.assertIn("submitted_before=10 submitted_during=11", trace)
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("submitted_before=10 submitted_during=11",
                                                                   "submitted_before=10 submitted_during=9"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("consumed_by=public_present_gpu_output",
                                                                   "consumed_by=discarded"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace, builtin=False)["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace, native=False)["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("frame=8 enabled=1 only_generated=1",
                                                                   "frame=8 enabled=1 only_generated=0"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("frame=14 width=96 height=80",
                                                                   "frame=14 width=128 height=128"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("stale_record=no_history_publication",
                                                                   "stale_record=published"))["pass"])
        self.assertIn("sync_qpc=200", trace)
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("sync_qpc=200", "sync_qpc=0", 1))["pass"])
        self.assertIn("sync_qpc=500", trace)
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("sync_qpc=500", "sync_qpc=100", 1))["pass"])
        self.assertIn("native_present=5", trace)
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("native_present=5", "native_present=1", 1))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("overflow=runtime_error", "overflow=ignored"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("debug=real_inputs", "debug=ignored"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("history=unmodified", "history=debug_pixels"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("frame=1 generated=0 output_index=0", "frame=1 generated=1 output_index=1"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("frame=3 generated=1 output_index=2", "frame=3 generated=1 output_index=1"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("frame=8 generated=1 output_index=4", "frame=8 generated=0 output_index=0"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("hash=0000000000000032", "hash=0000000000000031"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("reset_output=unchanged", "reset_output=copy_current"))["pass"])

    def test_fsr_real_three_channel_history_and_centered_cap_evidence(self):
        # Reduced authentic runtime02 GPU evidence; independent of runner counts.
        trace = """OWNED_D3D12_TEMPORAL_BACKEND id=owned-graphics-d3d12
OWNED_D3D12_FSR_FRAME_PASS frame=0 input_tag=96 sentinel=1
OWNED_D3D12_FSR_FRAME_PASS frame=1 input_tag=160 sentinel=1
OWNED_D3D12_FSR_FRAME_PASS frame=2 input_tag=96 sentinel=1
OWNED_D3D12_FSR_FRAME_PASS frame=3 input_tag=96 sentinel=1
OWNED_D3D12_FSR_REPLAY_PASS replay=0 input_tag=116 destroyed_before_submit=1 sentinel=1 changed_gpu_input=0
OWNED_D3D12_FSR_REPLAY_PASS replay=1 input_tag=116 destroyed_before_submit=1 sentinel=1 changed_gpu_input=0
OWNED_D3D12_FSR_REPLAY_PASS replay=2 input_tag=140 destroyed_before_submit=1 sentinel=1 changed_gpu_input=1
OWNED_D3D12_FSR_EXPOSURE_PASS format=0 value=1 output_mean=0.454901961 sentinel=1
OWNED_D3D12_FSR_EXPOSURE_PASS format=54 value=1 output_mean=0.454901961 sentinel=1
OWNED_D3D12_FSR_EXPOSURE_PASS format=41 value=1 output_mean=0.454901961 sentinel=1
OWNED_D3D12_FSR_CASE_PASS case=rejected_dispatch_output_unchanged result=0
OWNED_D3D12_FSR_FULL_SR_PASS motion=1 masks=1 transfer=1 sharpening_constant=1 output_generation=1
OWNED_D3D12_FSR_PASS provider=MetalFX image_quality_equivalence=not_claimed
"""
        behaviors = """case=auto_exposure_ignores_poison_texture max_byte_delta=0 sentinel=1
case=zero_motion_resolution_jitter_equivalence max_byte_delta=0 sentinel=1
case=auto_exposure_ignores_poison_texture max_byte_delta=0 sentinel=1
case=zero_motion_resolution_jitter_equivalence max_byte_delta=0 sentinel=1
case=auto_exposure_ignores_poison_texture max_byte_delta=0 sentinel=1
case=sharpen_constant_invariance max_byte_delta=0 sentinel=1
case=nonzero_motion_scale_equivalence max_byte_delta=0 sentinel=1
case=nonzero_motion_effect option=0 rgb_byte_delta_sum=13386 jitter_nonzero=0
case=nonzero_motion_scale_equivalence max_byte_delta=0 sentinel=1
case=nonzero_motion_effect option=2 rgb_byte_delta_sum=6939 jitter_nonzero=0
case=nonzero_motion_scale_equivalence max_byte_delta=0 sentinel=1
case=nonzero_motion_effect option=4 rgb_byte_delta_sum=5169 jitter_nonzero=1
case=sharpen_edge_alpha plain_variation=10752 sharp_variation=13888 alpha_unchanged=1
case=constant_transfer_roundtrip transfer=2 linear_max_error=0 alpha_unchanged=1 sentinel=1
case=transfer_independent_scene transfer=2 linear_mean_abs_error=0.00206413169 wrong_flag_byte_delta_sum=882 alpha_unchanged=1
case=constant_transfer_roundtrip transfer=4 linear_max_error=0.00109610761 alpha_unchanged=1 sentinel=1
case=transfer_independent_scene transfer=4 linear_mean_abs_error=0.00650057181 wrong_flag_byte_delta_sum=938 alpha_unchanged=1
case=reactive_composition_equivalence max_byte_delta=0 sentinel=1
case=mask_idempotence max_byte_delta=0 sentinel=1
case=history_mask_channel_equivalence max_byte_delta=0 sentinel=1
case=history_mask_channel_equivalence max_byte_delta=0 sentinel=1
case=reactive_history_effect rgb_byte_delta_sum=5829
case=composition_below_reactive_no_change max_byte_delta=0 sentinel=1
case=output_generation_switch_resets_history max_byte_delta=0 sentinel=1
case=history_reset_matches_fresh max_byte_delta=0 sentinel=1
case=cap_center_matches_native_scale_control max_byte_delta=0 sentinel=1
case=cap_scale_geometry native_max=3 content=192x192 caller=256x224 center=32,16 black_opaque=1 sentinel=1
case=inflight_release fence_gate=1 context_destroyed=1 caller_resources_released=1 sentinel=1
case=command_list_reset_discards_work output_unchanged=1 sentinel=1
case=exposure_gpu_replay replay=0 value=0.333299994 half_bits=3555 max_ulp=0 contexts_destroyed=1 gpu_produced_r32=1
case=exposure_gpu_replay replay=1 value=1.23456705 half_bits=3cf0 max_ulp=0 contexts_destroyed=1 gpu_produced_r32=1
case=exposure_gpu_replay replay=2 value=3.14159012 half_bits=4248 max_ulp=0 contexts_destroyed=1 gpu_produced_r32=1
"""
        trace += "".join("OWNED_D3D12_FSR_BEHAVIOR_PASS " + line + "\n" for line in behaviors.splitlines())
        self.assertTrue(self.simulate_metalfx("fsr", trace)["pass"])
        self.assertFalse(self.simulate_metalfx("fsr", trace.replace("center=32,16", "center=0,0"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr", trace.replace("max_ulp=0", "max_ulp=2"))["pass"])

    def test_fsr_summary_without_full_sr_and_centered_cap_evidence_cannot_pass(self):
        trace = "OWNED_D3D12_TEMPORAL_BACKEND id=owned-graphics-d3d12\nOWNED_D3D12_FSR_PASS\n"
        for frame in range(4):
            trace += f"OWNED_D3D12_FSR_FRAME_PASS frame={frame}\n"
        for replay, changed in ((0, 0), (1, 0), (2, 1)):
            trace += f"OWNED_D3D12_FSR_REPLAY_PASS replay={replay} pixels=exact destroyed_before_submit=1 sentinel=1 changed_gpu_input={changed}\n"
        for format in (0, 54, 41):
            trace += f"OWNED_D3D12_FSR_EXPOSURE_PASS format={format} value=1 pixels=exact sentinel=1\n"
        trace += "OWNED_D3D12_FSR_CASE_PASS case=rejected_dispatch_output_unchanged result=0\n"
        self.assertFalse(self.simulate_metalfx("fsr", trace)["pass"])

    def simulate(self, *, omit_frame: int | None = None, backend_log: str = "",
                 foreign: bool = False, timeout: bool = False, stderr_diagnostic: str = "",
                 api_validation: bool = False, compatibility_delta: int = 0,
                 omit_compatibility: bool = False) -> dict:
        with tempfile.TemporaryDirectory(dir=SCRATCH, prefix="runner-") as directory:
            base = Path(directory)
            runtime = base / "runtime"
            runtime.mkdir()
            (runtime / "owned-graphics-runtime.json").write_text('{"schema": 1}\n')
            fixture = base / "owned-d3d12-ngx.exe"
            fixture.write_bytes(controlled_pe_fixture())
            args = argparse.Namespace(runtime=runtime, lock=runtime / "owned-graphics-runtime.json",
                                      fixture=fixture, dxil_dir=base / "dxil", out=base / "result",
                                      suite="ngx", mode="no-present", validation=not api_validation,
                                      api_validation=api_validation, timeout=1)

            def fake_run(command, **kwargs):
                if command[0] == str(runtime / "bin/wine"):
                    # The NGX PE deliberately reuses the temporal backend guard.
                    kwargs["stdout"].write("OWNED_D3D12_TEMPORAL_BACKEND id=owned-graphics-d3d12\n")
                    for frame in range(8):
                        if frame != omit_frame:
                            kwargs["stdout"].write(f"OWNED_D3D12_NGX_FRAME_PASS frame={frame}\n")
                    kwargs["stdout"].write("OWNED_D3D12_NGX_BEHAVIOR_PASS case=rejected_output_preserved "
                                           "invalid_input_bounds=1 invalid_compatibility_scalars=5 sentinel=1\n")
                    if not omit_compatibility:
                        for name, scale, x, y, highres in (
                                ("exposure_scale_auto", 2, 0, 0, 0),
                                ("exposure_scale_texture", 2, 0, 0, 0),
                                ("exposure_scale_no_texture", 0.375, 0, 0, 0),
                                ("mv_value_offsets_lowres", 1, -1.25, 2.5, 0),
                                ("mv_value_offsets_highres_jittered", 1, -0.375, 0.625, 1)):
                            for replay in range(3):
                                kwargs["stdout"].write(
                                    f"OWNED_D3D12_NGX_BEHAVIOR_PASS case={name} replay={replay} "
                                    f"exposure_scale={scale} mv_offset_x={x} mv_offset_y={y} "
                                    f"max_byte_delta={compatibility_delta} sentinel=1 live_inputs=1 "
                                    f"params_destroyed=1 released_before_submit=1 highres_jittered={highres}\n")
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
            (runtime / "owned-graphics-runtime.json").write_text('{"schema": 1}\n')
            if native:
                (runtime / "lib/wine/x86_64-unix/winemetal.so").write_bytes(b"native")
            fixture = base / "owned-d3d12-dxil.exe"
            fixture.write_bytes(controlled_pe_fixture())
            dxil = base / "dxil"
            dxil.mkdir()
            if shaders:
                for name in runner.DXIL_SHADERS + runner.DXIL_REGRESSION_SHADERS:
                    (dxil / name).write_bytes(b"dxil-" + name.encode())
            args = argparse.Namespace(runtime=runtime, lock=runtime / "owned-graphics-runtime.json",
                                      fixture=fixture, dxil_dir=dxil, out=base / "result",
                                      suite="dxil", mode="no-present", validation=False,
                                      api_validation=False, timeout=1)

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
                    kwargs["stdout"].write("OWNED_D3D12_DXIL_STATIC_BOUNDS_PASS null_reads=zero "
                                           "oob_atomic=zero oob_writes=dropped sentinels=preserved "
                                           "in_range=exact root_uav=exact\n")
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
                            procedural_clobbered_commit: bool = False,
                            contract_trace: str = RAYTRACING_CONTRACT_TRACE,
                            stderr_diagnostic: str = "") -> dict:
        with tempfile.TemporaryDirectory(dir=SCRATCH, prefix="raytracing-") as directory:
            base = Path(directory)
            runtime = base / "runtime"
            (runtime / "lib/wine/x86_64-unix").mkdir(parents=True)
            (runtime / "owned-graphics-runtime.json").write_text('{"schema": 1}\n')
            (runtime / "lib/wine/x86_64-unix/winemetal.so").write_bytes(b"native")
            fixture = base / "owned-d3d12-raytracing.exe"
            fixture.write_bytes(controlled_pe_fixture())
            dxil = base / "dxil"
            dxil.mkdir()
            if shaders:
                for name in runner.RAYTRACING_SHADERS:
                    (dxil / name).write_bytes(b"dxil-" + name.encode())
            args = argparse.Namespace(runtime=runtime, lock=runtime / "owned-graphics-runtime.json",
                                      fixture=fixture, dxil_dir=dxil, out=base / "result",
                                      suite="raytracing", mode="no-present", validation=False,
                                      api_validation=False, timeout=1)

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
                    out.write(contract_trace)
                    out.write(f"{runner.RAYTRACING_PASS_MARKER}\n")
                    if loaded:
                        kwargs["stderr"].write(f"dyld[1]: {runtime}/lib/wine/x86_64-unix/winemetal.so\n")
                    kwargs["stderr"].write(stderr_diagnostic)
                return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

            with patch.object(runner.subprocess, "run", side_effect=fake_run), \
                    contextlib.redirect_stdout(io.StringIO()):
                result = runner.run(args)
            report = json.loads((args.out / "result.json").read_text())
            self.assertEqual(result, 0 if report["pass"] else 1)
            return report

    def test_valid_raytracing_run_passes(self):
        report = self.simulate_raytracing()
        self.assertTrue(report["pass"], report["raytracing"])
        self.assertTrue(report["dxil_native_compiler_verified"])
        self.assertEqual(report["raytracing"]["lanes"], list(range(runner.RAYTRACING_LANE_COUNT)))

    def test_raytracing_without_frozen_shader_input_cannot_run(self):
        with self.assertRaisesRegex(ValueError, "regular file"):
            self.simulate_raytracing(shaders=False)

    def test_raytracing_requires_one_completed_hitobject_oracle(self):
        marker = "OWNED_D3D12_RAYTRACING_CASE hitobject status=PASS\n"
        self.assertTrue(self.simulate_raytracing()["pass"])
        for replacement in ("", marker + marker, marker.replace("PASS", "FAIL")):
            with self.subTest(replacement=replacement):
                report = self.simulate_raytracing(contract_trace=RAYTRACING_CONTRACT_TRACE.replace(marker, replacement))
                self.assertFalse(report["pass"])
                self.assertTrue(report["raytracing"]["failures"])

    def test_raytracing_reorder_scope_requires_all_actual_words_and_multiple_waves(self):
        trace = RAYTRACING_CONTRACT_TRACE
        row = coherence_control_rows[0]
        summary = "OWNED_D3D12_RAYTRACING_HITOBJECT_COHERENCE lanes=64 words=512 scope=reorder wave_max=32 status=PASS\n"
        marker = "OWNED_D3D12_RAYTRACING_CASE hitobject-reorder-coherence status=PASS\n"
        self.assertTrue(self.simulate_raytracing(contract_trace=trace)["pass"])
        bad_traces = (
            trace.replace(marker, ""), trace + marker, trace.replace(marker, marker.replace("PASS", "FAIL")),
            trace.replace(summary, ""), trace + summary, trace.replace("scope=reorder", "scope=device"),
            trace.replace("words=512", "words=511"), trace.replace("wave_max=32", "wave_max=64"),
            trace.replace(row, ""), trace + row,
            trace.replace(coherence_control_rows[0] + coherence_control_rows[1],
                          coherence_control_rows[1] + coherence_control_rows[0]),
        )
        prefix, _, word_text = row.partition("(")
        for word in range(8):
            corrupted = word_text.removesuffix(")\n").split(",")
            corrupted[word] = "0xffffffff"
            bad_traces += (trace.replace(row, prefix + "(" + ",".join(corrupted) + ")\n"),)
        for index, bad_trace in enumerate(bad_traces):
            with self.subTest(mutation=index):
                report = self.simulate_raytracing(contract_trace=bad_trace)
                self.assertFalse(report["pass"])
                self.assertTrue(report["raytracing"]["failures"])

    def test_atomic64_producers_requires_actual_returns_counts_and_primitives(self):
        # Synthetic parser stimuli only, never GPU evidence; independent of runner case constants.
        cases = (("vs", (6, 0, 0, 0, 0, 0), 2), ("hs", (0, 6, 0, 0, 0, 0), 2),
                 ("ds", (0, 0, 6, 0, 0, 0), 2), ("gs", (0, 0, 0, 4, 0, 0), 4),
                 ("ds_gs", (0, 0, 6, 4, 0, 0), 4), ("ms_grid", (0, 0, 0, 0, 0, 16), 4),
                 ("as_child1", (0, 0, 0, 0, 8, 8), 2), ("as_child2", (0, 0, 0, 0, 8, 16), 4),
                 ("as_child3d", (0, 0, 0, 0, 8, 32), 8))
        rows = []
        for name, counts, primitives in cases:
            for samples in (1, 4):
                for stage in range(6):
                    for lane in range(64):
                        active = lane < counts[stage]
                        initial = 0x1234567800000000 + (stage * 64 + lane) * 256
                        old, cas = (initial, initial + 1) if active else (0xcdcdcdcdcdcdcdcd,) * 2
                        rows.append(f"OWNED_D3D12_ATOMIC64_PRODUCERS_RECORD case={name} samples={samples} "
                                    f"stage={stage} lane={lane} old={old:016x} cas={cas:016x} count={int(active)} "
                                    f"cell={initial + (9 if active else 0):016x} guard=cdcdcdcd,cdcdcdcd,cdcdcdcd\n")
                rgba = (primitives * 16 << 24) | (primitives * (10 if samples == 4 else 4) << 8)
                for index in range(64):
                    rows.append(f"OWNED_D3D12_ATOMIC64_PRODUCERS_PIXEL case={name} samples={samples} index={index} rgba={rgba:08x}\n")
                rows.append(f"OWNED_D3D12_ATOMIC64_PRODUCERS_CASE case={name} samples={samples} status=PASS\n")
        summary = "OWNED_D3D12_ATOMIC64_PRODUCERS_PASS cases=18 stages=VS,HS,DS,GS,AS,MS\n"
        trace = "".join(rows) + summary
        self.assertFalse(runner.atomic64_producers_failures(trace, ""))
        for invalid in (summary, trace.replace(rows[0], ""), trace + rows[0], trace + summary,
                        trace.replace("old=1234567800000000", "old=1234567700000000", 1),
                        trace.replace("cas=1234567800000001", "cas=1234567800000000", 1),
                        trace.replace("count=1", "count=2", 1), trace.replace("count=0", "count=1", 1),
                        trace.replace("cell=1234567800000009", "cell=1234567800000000", 1),
                        trace.replace("guard=cdcdcdcd", "guard=00000000", 1),
                        trace.replace("rgba=20000800", "rgba=10000400", 1),
                        trace.replace("rgba=20001400", "rgba=20000800", 1)):
            self.assertTrue(runner.atomic64_producers_failures(invalid, ""))
        self.assertTrue(runner.atomic64_producers_failures(trace, "kIOGPUCommandBufferCallbackError"))
        self.assertTrue(runner.atomic64_producers_failures(trace, "OWNED_D3D12_SM6_MISMATCH"))

    def test_atomic64_typed_requires_positive_cap_and_complete_matrix(self):
        # Controlled parser stimuli only, not a GPU capture.
        prefix = "OWNED_D3D12_ATOMIC64_TYPED"
        cap = prefix + "_CAP typed=1\n"
        rows = [cap]
        for shape in range(6):
            for sign in range(2):
                rows.extend((f"{prefix}_CASE shape={shape} signed={sign}\n",
                             f"{prefix}_BEGIN shape={shape} signed={sign} first=0\n",
                             "OWNED_D3D12_SM6_CASE_PASS atomic64_typed_operations\n",
                             "OWNED_D3D12_SM6_CASE_PASS atomic64_typed_contention\n"))
                if shape == sign == 0:
                    rows.append("OWNED_D3D12_SM6_CASE_PASS atomic64_typed_raw32_aliases\n")
        formats = ((65, 1), (61, 1), (63, 1), (62, 1), (64, 1),
                   (56, 2), (58, 2), (57, 2), (59, 2), (54, 2),
                   (49, 2), (51, 2), (50, 2), (52, 2), (42, 4), (43, 4), (41, 4),
                   (35, 4), (37, 4), (36, 4), (38, 4), (34, 4),
                   (28, 4), (31, 4), (30, 4), (32, 4), (24, 4), (25, 4), (26, 4),
                   (17, 8), (18, 8), (16, 8), (11, 8), (13, 8), (12, 8), (14, 8), (10, 8),
                   (3, 16), (4, 16), (2, 16))
        for format_id, unit in formats:
            rows.extend((f"{prefix}_FORMAT format={format_id} unit={unit}\n",
                         "OWNED_D3D12_SM6_CASE_PASS atomic64_typed_format\n"))
        rows.extend(("OWNED_D3D12_SM6_CASE_PASS atomic64_typed_float32_aliases\n",
                     prefix + "_PASS shapes=6 signs=2 failures=0\n"))
        trace = "".join(rows)
        self.assertFalse(runner.atomic64_typed_failures(trace, ""))
        for invalid in (trace.replace(cap, ""), trace.replace("typed=1", "typed=0"),
                        trace + cap, trace.replace("typed=1", "typed=1 diagnostic=1"),
                        trace.replace("first=0", "first=1", 1),
                        trace.replace("OWNED_D3D12_SM6_CASE_PASS atomic64_typed_contention\n", "", 1)):
            self.assertTrue(runner.atomic64_typed_failures(invalid, ""))


    def test_workgraphs_atomic64_typed_requires_physical_publication(self):
        # Controlled parser stimuli only, not a GPU capture.
        rows = []
        for index in range(16):
            old = (0xfffffffafffff000 + index * 0x100000001) % (1 << 64)
            final = (old + 0x40000000f) % (1 << 64)
            rows.append(f"OWNED_D3D12_ATOMIC64_WG_TYPED_TEXEL index={index} old=0x{old:016x} "
                        f"final=0x{final:016x} copy=0x{final:016x}\n")
        trace = ("".join(rows) + "OWNED_D3D12_ATOMIC64_WG_TYPED_CAP library_profile=lib_6_8 typed_cap=1\n"
                 "OWNED_D3D12_ATOMIC64_WG_TYPED_COUNTS count=16 guard=a11c0ded\n"
                 "OWNED_D3D12_ATOMIC64_WG_TYPED_PASS texels=16 full64_words=48 nonaliasing_views=1 API_barrier=1 physical_copy=PASS count=16 guard=PASS\n")
        self.assertFalse(runner.workgraphs_atomic64_typed_failures(trace, ""))
        for invalid in (trace.replace(rows[0], ""), trace.replace("copy=0x", "copy=0x0", 1),
                        trace.replace("count=16 guard=a11c0ded", "count=15 guard=a11c0ded", 1),
                        trace.replace("typed_cap=1", "typed_cap=0")):
            self.assertTrue(runner.workgraphs_atomic64_typed_failures(invalid, ""))

    def test_workgraphs_atomic64_exact_values_and_both_modes(self):
        # Controlled parser stimuli only; these rows are not GPU execution evidence.
        rows = []
        for mode in ("CPU", "GPU"):
            for index in range(16):
                initial = (0xfffffffafffff000 + index * 0x100000001) % (1 << 64)
                added = (initial + 0x40000000f) % (1 << 64)
                before_xor = added ^ 0xa5c35a3cf00d1234
                after_xor = before_xor ^ 0x13579bdf2468ace0
                after_cas = (after_xor + 0x40000000f) % (1 << 64)
                guard = 0xf00d1234 ^ index
                words = (initial, added, before_xor, after_xor, after_xor, after_cas,
                         guard | ((guard + 3) << 32), index, after_cas ^ 0x55aa55aaaa55aa55,
                         0x8000000000000005 + index, index | ((guard + 3) << 32), 0xdeadbeefcafef00d)
                rows.append(f"OWNED_D3D12_ATOMIC64_WORKGRAPH_RECORD mode={mode} index={index} "
                            + "words=(" + ",".join(f"0x{word:016x}" for word in words) + ")\n")
            rows.append(f"OWNED_D3D12_ATOMIC64_WORKGRAPH_COUNTS mode={mode} seed=16 child=16 terminal=16 guard=a11c0ded\n")
            rows.append(f"OWNED_D3D12_ATOMIC64_WORKGRAPH_PASS mode={mode} records=16 fields=192 seed_count=16 "
                        "child_count=16 terminal_count=16 input_bytes=16 input_align=8 queue_guard=PASS old_returns=PASS final_values=PASS\n")
        trace = ("OWNED_D3D12_ATOMIC64_WORKGRAPH_CAP library_profile=lib_6_8 advertised_shader_model=67 "
                 "typed_cap=1 heap_cap=1\n" + "".join(rows) +
                 "OWNED_D3D12_ATOMIC64_WORKGRAPH_TOTAL_PASS modes=2 fields=384\n")
        self.assertFalse(runner.workgraphs_atomic64_failures(trace, ""))
        corrupt = trace.replace("0xdeadbeefcafef00d", "0xdeadbeefcafef00c", 1)
        missing_gpu = "\n".join(line for line in trace.splitlines() if "mode=GPU" not in line)
        for invalid in (corrupt, missing_gpu, trace + rows[0], trace.replace("guard=a11c0ded", "guard=a11c0dec", 1),
                        trace.replace("typed_cap=1", "typed_cap=0"), trace.replace("heap_cap=1", "heap_cap=0")):
            self.assertTrue(runner.workgraphs_atomic64_failures(invalid, ""))
        self.assertTrue(runner.workgraphs_atomic64_failures(trace, "kIOGPUCommandBufferCallbackError"))

    def test_raytracing_atomic64_requires_all_returned_words(self):
        # Controlled CPU parser stimuli only, never GPU execution evidence.
        rows = []
        for lane in range(64):
            seed = ((0x24680000 + lane) << 32) | (0xfedc0000 + lane)
            value = seed
            value += 0x200000021
            stage_old = value
            tag = 0x73 if lane & 1 else 0x35
            value += 0x100000007 + tag
            payload = (((lane * 3 + 5) * 3) ^ (stage_old & 0xffffffff) ^ tag)
            cas_old = value
            value ^= 0x0102030405060708
            payload ^= (cas_old & 0xffffffff) ^ 0x91
            wide = (value, seed, value, (payload << 32) | (2 if lane & 1 else 1))
            words = [component for item in wide for component in (item & 0xffffffff, item >> 32)]
            rows.append(f"OWNED_D3D12_RAYTRACING_ATOMIC64_LANE lane={lane} words=("
                        + ",".join(f"0x{word:08x}" for word in words) + ")\n")
        summary = "OWNED_D3D12_RAYTRACING_ATOMIC64 lanes=64 words=512 stages=raygen,closesthit,miss,callable status=PASS\n"
        marker = "OWNED_D3D12_RAYTRACING_CASE atomic64-rt status=PASS\n"
        completed = "OWNED_D3D12_RAYTRACING_ATOMIC64_PASS\n"
        extended_rows = []
        extended_summaries = []
        shared_rows = []
        for mode in range(2):
            for lane in range(24):
                kind = lane % 3
                seed = ((0x24680000 + lane) << 32) | (0xfedc0000 + lane)
                value = seed + 0x200000021
                intersection = any_hit = 0
                if kind == 1:
                    intersection = value
                    value += 0x400000013
                if kind != 2:
                    any_hit = value
                    value += 0x800000017
                stage = value
                tag = (0x35, 0x57, 0x73)[kind]
                value += (0x300000009 if kind == 2 else 0x100000007) + tag
                ticket = ((1 << 64) - 1 + (23 - lane) * 0x100000001) & ((1 << 64) - 1)
                wide = (value ^ 0x0102030405060708, seed, any_hit, intersection, stage, value, ticket,
                        2 if kind == 2 else 1, 0x1000091 + (0x20000 if kind == 2 else 0x10001), tag)
                words = [part for item in wide for part in (item & 0xffffffff, item >> 32)]
                extended_rows.append(f"OWNED_D3D12_RAYTRACING_ATOMIC64_EXT_LANE mode={mode} lane={lane} words=("
                                     + ",".join(f"0x{word:08x}" for word in words) + ")\n")
            shared_rows.append(f"OWNED_D3D12_RAYTRACING_ATOMIC64_EXT_SHARED mode={mode} words=(0x00000017,0x00000018,0x0000005f,0x00000000)\n")
            extended_summaries.append(f"OWNED_D3D12_RAYTRACING_ATOMIC64_EXT mode={mode} lanes=24 words=484 local=cbv stages=96 tickets=24 status=PASS\n")
        trace = "".join(rows + extended_rows + shared_rows + extended_summaries) + summary + completed + marker
        self.assertFalse(runner.raytracing_atomic64_failures(trace, ""))
        bad_traces = (
            trace.replace(marker, ""), trace + marker, trace.replace(marker, marker.replace("PASS", "FAIL")),
            trace.replace(summary, ""), trace + summary, trace.replace("words=512", "words=511"),
            trace.replace("stages=raygen,closesthit,miss,callable", "stages=raygen"),
            trace.replace(completed, ""), trace + completed, trace.replace(rows[0], ""), trace + rows[0],
            trace.replace(rows[0] + rows[1], rows[1] + rows[0]),
            trace + "OWNED_D3D12_RAYTRACING_ATOMIC64_LANE malformed\n",
        )
        for index, bad_trace in enumerate(bad_traces):
            with self.subTest(structure=index):
                self.assertTrue(runner.raytracing_atomic64_failures(bad_trace, ""))
        for lane, row in enumerate(rows):
            prefix, _, suffix = row.partition("(")
            words = suffix.removesuffix(")\n").split(",")
            for word in range(8):
                corrupted = words.copy()
                corrupted[word] = f"0x{int(corrupted[word], 16) ^ 1:08x}"
                with self.subTest(lane=lane, word=word):
                    changed = trace.replace(row, prefix + "(" + ",".join(corrupted) + ")\n")
                    self.assertTrue(runner.raytracing_atomic64_failures(changed, ""))
        self.assertTrue(runner.raytracing_atomic64_failures(trace, "kIOGPUCommandBufferCallbackError"))
        for row in extended_rows + shared_rows:
            prefix, _, suffix = row.partition("(")
            words = suffix.removesuffix(")\n").split(",")
            for word in range(len(words)):
                corrupted = words.copy()
                corrupted[word] = f"0x{int(corrupted[word], 16) ^ 1:08x}"
                with self.subTest(extended=prefix, word=word):
                    changed = trace.replace(row, prefix + "(" + ",".join(corrupted) + ")\n")
                    self.assertTrue(runner.raytracing_atomic64_failures(changed, ""))
        for row in extended_rows + shared_rows + extended_summaries:
            with self.subTest(missing=row):
                self.assertTrue(runner.raytracing_atomic64_failures(trace.replace(row, ""), ""))
            with self.subTest(duplicate=row):
                self.assertTrue(runner.raytracing_atomic64_failures(trace + row, ""))
        self.assertTrue(runner.raytracing_atomic64_failures(trace.replace("local=cbv", "local=constant"), ""))

    def test_raytracing_complete_lanes_cannot_replace_missing_contract_evidence(self):
        markers = (
            "FORMAT r8g8_snorm", "CASE geometry-transform-row-major", "CASE geometry-rules", "CONTRACT failures=0",
            "CASE add-to-state-object", "CASE generic-function-collection", "STATEOBJ failures=0",
            "RAYQUERY_CASE raygen-indirect", "RAYQUERY failures=0",
            "RAYQUERY_CASE hs", "RAYQUERY_CASE ds", "RAYQUERY_CASE gs",
            "RAYQUERY_CASE as", "RAYQUERY_CASE ms", "RAYQUERY_CASE closesthit",
            "RAYQUERY_CASE anyhit", "RAYQUERY_CASE intersection",
            "RAYQUERY_CASE miss", "RAYQUERY_CASE callable", "RAYQUERY_STAGES failures=0",
            "ROOTARGS_CASE dispatch-rays-root-arguments",
            "COVERAGE_FEATURE rayquery-template-skip", "COVERAGE_CASE grid-indirect",
            "COVERAGE_PREDICATION predication-indirect-ne0-1",
            "COVERAGE_BUNDLE bundle-direct-predicated-run", "COVERAGE failures=0",
        )
        for marker in markers:
            with self.subTest(marker=marker):
                trace = "\n".join(line for line in RAYTRACING_CONTRACT_TRACE.splitlines()
                                  if not line.startswith("OWNED_D3D12_RAYTRACING_" + marker)) + "\n"
                report = self.simulate_raytracing(contract_trace=trace)
                self.assertFalse(report["pass"], report["raytracing"])
                self.assertEqual(report["raytracing"]["failures"], [])
                self.assertTrue(report["dxil_native_compiler_verified"])

    def test_raytracing_corrupt_contract_counts_and_culling_cannot_pass(self):
        mutations = (
            ("raygen-indirect lanes=24", "raygen-indirect lanes=23"),
            ("intersection lanes=24", "intersection lanes=23"),
            ("dispatch-rays-root-arguments slots=4", "dispatch-rays-root-arguments slots=3"),
            ("geometry-index lanes=24", "geometry-index lanes=23"),
            ("predication-direct-eq0-0 culled=1", "predication-direct-eq0-0 culled=0"),
            ("bundle-indirect-predicated-culled culled=1",
             "bundle-indirect-predicated-culled culled=0"),
        )
        for before, after in mutations:
            with self.subTest(evidence=before):
                report = self.simulate_raytracing(
                    contract_trace=RAYTRACING_CONTRACT_TRACE.replace(before, after))
                self.assertFalse(report["pass"], report["raytracing"])
                self.assertEqual(report["raytracing"]["failures"], [])

    def test_raytracing_contract_mismatch_cannot_hide_behind_pass_markers(self):
        for marker in ("CASE_MISMATCH", "RAYQUERY_MISMATCH", "ROOTARGS_MISMATCH"):
            with self.subTest(marker=marker):
                report = self.simulate_raytracing(
                    stderr_diagnostic=f"OWNED_D3D12_RAYTRACING_{marker} actual=corrupt\n")
                self.assertFalse(report["pass"], report["raytracing"])
                self.assertEqual(report["raytracing"]["failures"], [])

    def test_raytracing_missing_or_corrupt_as_evidence_cannot_pass(self):
        mutations = (
            ("RAYTRACING_AS case=serialize-tlas-relocate failures=0 status=PASS\n", ""),
            ("RAYTRACING_AS case=postbuild-inline failures=0 status=PASS\n"
             "RAYTRACING_AS case=postbuild-emit failures=0 status=PASS\n",
             "RAYTRACING_AS case=postbuild-emit failures=0 status=PASS\n"
             "RAYTRACING_AS case=postbuild-inline failures=0 status=PASS\n"),
            ("RAYTRACING_AS_SUMMARY cases=16 failures=0\n", ""),
            ("RAYTRACING_AS_SUMMARY cases=16", "RAYTRACING_AS_SUMMARY cases=15"),
            ("case=update-tlas-copy failures=0", "case=update-tlas-copy failures=1"),
            ("RAYTRACING_AS_SUMMARY cases=16 failures=0\n",
             "RAYTRACING_AS_SUMMARY cases=16 failures=0\n" * 2),
            ("RAYTRACING_AS_SUMMARY cases=16 failures=0\n",
             "RAYTRACING_AS_SUMMARY cases=16 failures=0\n"
             "RAYTRACING_AS case=serialize-blas failures=1 status=FAIL\n"),
        )
        for before, after in mutations:
            with self.subTest(evidence=before):
                report = self.simulate_raytracing(
                    contract_trace=RAYTRACING_CONTRACT_TRACE.replace(before, after))
                self.assertFalse(report["pass"], report["raytracing"])
                self.assertEqual(report["raytracing"]["failures"], [])

    def test_raytracing_as_failure_cannot_hide_behind_complete_evidence(self):
        report = self.simulate_raytracing(stderr_diagnostic="RAYTRACING_AS FAIL case=serialize-blas\n")
        self.assertFalse(report["pass"], report["raytracing"])
        self.assertEqual(report["raytracing"]["failures"], [])

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

    def test_ngx_ignored_compatibility_requires_unchanged_gpu_output(self):
        self.assertFalse(self.simulate(compatibility_delta=2)["pass"])
        self.assertFalse(self.simulate(omit_compatibility=True)["pass"])

    def test_missing_frame_cannot_pass(self):
        self.assertFalse(self.simulate(omit_frame=4)["pass"])

    def test_error_only_in_backend_file_cannot_pass(self):
        report = self.simulate(backend_log="err: Device error: Metal command buffer failed\n")
        self.assertFalse(report["pass"])
        self.assertTrue(report["validation_errors"])

    def test_api_only_validation_still_rejects_backend_error(self):
        report = self.simulate(backend_log="err: Device error: Metal command buffer failed\n",
                               api_validation=True)
        self.assertFalse(report["pass"])
        self.assertEqual(report["validation_mode"], "api-only")
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

    def test_atomic_copy_inconclusive_progress_is_core_pass_not_live_pass(self):
        # Shader instrumentation can serialize independent queues. All ordered
        # payloads still pass, but that is not evidence of live reader progress.
        for suite in ("atomic-copy", "atomic-copy-live"):
            with self.subTest(suite=suite), tempfile.TemporaryDirectory(dir=SCRATCH, prefix="atomic-") as directory:
                base = Path(directory)
                runtime = base / "runtime"
                native = runtime / "lib/wine/x86_64-unix/winemetal.so"
                native.parent.mkdir(parents=True)
                native.write_bytes(b"native")
                (runtime / "owned-graphics-runtime.json").write_text('{"schema": 1}\n')
                fixture = base / "owned-d3d12-atomic-copy.exe"
                fixture.write_bytes(controlled_pe_fixture())
                dxil = base / "dxil"
                dxil.mkdir()
                (dxil / "atomic-copy-reader.dxil").write_bytes(b"reader")
                args = argparse.Namespace(runtime=runtime, lock=runtime / "owned-graphics-runtime.json",
                                          fixture=fixture, dxil_dir=dxil, out=base / "result",
                                          suite=suite, mode="no-present", validation=False,
                                          api_validation=True, timeout=1)

                def fake_run(command, **kwargs):
                    if command[0] == str(runtime / "bin/wine"):
                        lines = ["OWNED_D3D12_ATOMIC_COPY_BACKEND id=owned-graphics-d3d12"]
                        for name in ("direct", "compute", "copy"):
                            lines.extend((f"OWNED_D3D12_ATOMIC_COPY_CASE_PASS list={name} "
                                          "uint32=1 dependencies=0,1 ordered=1",
                                          f"OWNED_D3D12_ATOMIC_COPY64_CASE_PASS list={name} "
                                          "scenarios=6 dependencies=0,2 exact=1"))
                        lines.extend(("OWNED_D3D12_ATOMIC_COPY64_COMPUTE_PASS list=0 "
                                      "predication_false=1 payload=1",
                                      "OWNED_D3D12_ATOMIC_COPY64_COMPUTE_PASS list=2 "
                                      "predication_false=0 payload=1",
                                      "OWNED_D3D12_ATOMIC_COPY64_LIFETIME_PASS replay=2 released_list=1",
                                      "OWNED_D3D12_ATOMIC_COPY64_LIVE_INCONCLUSIVE observed=256 "
                                      "changed_threads=0 first=0 last=128 atomic64_snapshot=unproven",
                                      "OWNED_D3D12_ATOMIC_COPY64_CONTROL_PASS threads=256 final=128 payload=1",
                                      "OWNED_D3D12_ATOMIC_COPY_VALIDATION_PASS cases=33",
                                      "OWNED_D3D12_ATOMIC_COPY_PASS uint32=1 uint64=1"))
                        kwargs["stdout"].write("\n".join(lines) + "\n")
                        kwargs["stderr"].write(f"dyld[1]: {native}\n")
                    return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

                with patch.object(runner.subprocess, "run", side_effect=fake_run), contextlib.redirect_stdout(io.StringIO()):
                    result = runner.run(args)
                report = json.loads((args.out / "result.json").read_text())
                self.assertEqual(report["pass"], suite == "atomic-copy")
                self.assertEqual(result, 0 if suite == "atomic-copy" else 1)


class Atomic64PhaseEvidence(unittest.TestCase):
    def test_numeric_evidence_rejects_corruption(self):
        # Controlled parser input, NOT GPU evidence. Derive carry into the high
        # word separately so copying the fixture verdict cannot make this pass.
        prefix = "OWNED_D3D12_ATOMIC64_PHASE_"
        lines = [prefix + "BACKEND id=owned-graphics-d3d12",
                 prefix + "MAP iteration=0 words=5555555500000000,5555555600000001,8765432100000001,8765432100000001,123456789abcdef0,123456789abcdef0",
                 prefix + "MAP iteration=1 words=1234567800000003,1234567900000004,8765432100000004,8765432100000004,123456789abcdef0,123456789abcdef0"]
        names = ("indirect_nonzero", "indirect_count_zero", "indirect_dimension_zero", "indirect_predicate_zero",
                 "crossqueue_atomic_to_ordinary", "crossqueue_ordinary_to_atomic")
        for case, name in enumerate(names):
            if case >= 4:
                before, after, done = ((0, 10, 2), (3, 7, 5))[case - 4]
                lines.append(prefix + f"ASYNC case={name} pending={before} completed={done - 1} submitted={done} event=258")
                lines.append(prefix + f"FENCE case={name} before={before} after={after} rewind=3 event=0 wait_first=1")
            data = bytearray()
            for cell in range(8):
                active = case not in (1, 2, 3) and cell < 4
                high = 0x12345678 + cell
                low = 0xfffffff0 + cell
                initial = f"{high:08x}{low:08x}"
                if active:
                    low += 65 if case == 0 and cell >= 2 else 33
                    high += 1 + (low >> 32)
                    low &= 0xffffffff
                final = f"{high:08x}{low:08x}"
                data.extend(struct.pack("<II", low, high))
                if cell < 4:
                    old, returned = (initial, final) if active else ("cdcdcdcdcdcdcdcd",) * 2
                    root = ("face0001" if case == 0 and cell >= 2 else "face0000") if active else "cdcdcdcd"
                    lines.append(prefix + f"RETURN case={name} index={cell} old=0x{old} final=0x{returned} root={root}")
                lines.append(prefix + f"CELL case={name} index={cell} copy=0x{final} ordinary=0x{final}")
            lines.append(prefix + f"COUNTS case={name} count={0 if case in (1, 2, 3) else 4} guard=a11c0ded")
            for index, offset in enumerate((4, 8, 12, 16, 4, 8, 4)):
                word, = struct.unpack_from("<I", data, offset)
                lines.append(prefix + f"RAW case={name} index={index} word={word:08x}")
            if case < 4:
                commands = [[0, 33, 0xface0000, 0, 2, 1, 1], [2, 65, 0xface0001, 0, 1, 2, 1],
                            [4, 65, 0xface0002, 0, 1, 1, 1]]
                if case == 2:
                    for command in commands:
                        command[4] = 0
                for index, word in enumerate(sum(commands, [])):
                    lines.append(prefix + f"ARGUMENT case={name} index={index} word={word:08x}")
                lines.append(prefix + f"CONTROL case={name} count={0 if case == 1 else 2} predicate={0 if case == 3 else 1}")
        lines.extend((prefix + "RESULT failures=0", prefix + "PASS indirect=4 crossqueue=2"))
        stdout = "\n".join(lines)
        self.assertEqual(runner.atomic64_phase_failures(stdout), [])
        # Flip each printed numeric field independently (old, final, copied and
        # ordinary value, root, arguments, count, guard, predicates and fences).
        for index, line in enumerate(lines[1:]):
            if "=" not in line:
                continue
            fields = line.split()
            for field_index, field in enumerate(fields):
                if "=" not in field or field.startswith("case="):
                    continue
                changed = fields.copy()
                changed[field_index] = field[:-1] + ("1" if field[-1] != "1" else "0")
                corrupted = lines.copy()
                corrupted[index + 1] = " ".join(changed)
                with self.subTest(line=index, field=field.split("=")[0]):
                    self.assertTrue(runner.atomic64_phase_failures("\n".join(corrupted)))
        self.assertTrue(runner.atomic64_phase_failures("\n".join(lines[:-3] + lines[-2:])))
        self.assertTrue(runner.atomic64_phase_failures(stdout + "\n" + lines[3]))
        self.assertTrue(runner.atomic64_phase_failures("\n".join(lines[-2:])))
        self.assertTrue(runner.atomic64_phase_failures(stdout, "OWNED_D3D12_SM6_FATAL timeout"))


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
    add("encode", queue=1, submission=10, list=54, recording=1, encoder=501)
    add("temporal_encode", queue=1, submission=10, encoder=501, evaluation=1, feature=3, generation=4,
        color=11, output=21, history_sequence=1, reset=1, caller_reset=0)
    add("submit", queue=1, submission=10)
    add("submit", queue=1, submission=11)
    add("signal_committed", queue=1, submission=11, fence=80, generation=1, value=1)
    add("wait", queue=2, fence=80, generation=1, value=1)
    add("execute_list", queue=2, submission=20, list=55, recording=1)
    add("encode", queue=2, submission=20, list=55, recording=1, encoder=502)
    add("temporal_encode", queue=2, submission=20, encoder=502, evaluation=2, feature=3, generation=4,
        color=12, output=22, history_sequence=2, reset=0, caller_reset=0)
    add("submit", queue=2, submission=20)
    add("complete", queue=1, submission=10, status=4)
    add("complete", queue=1, submission=11, status=4)
    add("complete", queue=2, submission=20, status=4)
    add("temporal_frame_destroy", evaluation=1)
    add("temporal_frame_destroy", evaluation=2)
    return events


class QueryOrderingFailures(unittest.TestCase):
    def setUp(self):
        self.values = [(100 + i * 3, 101 + i * 3, 102 + i * 3) for i in range(8)]
        self.copy_values = [(100 + i * 4, 101 + i * 4, 102 + i * 4, 101 + i * 4, 103 + i * 4) for i in range(8)]

    def trace(self):
        return ("OWNED_D3D12_QUERY_BOP_PASS rounds=8 separate_execute=1 same_slot_reuse=1 "
                "inter_submission_cpu_waits=0\n" + "".join(
                    f"OWNED_D3D12_QUERY_BOP_VALUES round={i} end={end} successor={successor} reused={reused}\n"
                    for i, (end, successor, reused) in enumerate(self.values))
                + "OWNED_D3D12_QUERY_COPY_CAP supported=1\n"
                + "OWNED_D3D12_QUERY_COPY_CALIBRATION frequency=24000000 gpu_before=90 gpu_after=140 "
                  "cpu_before=1000 cpu_after=1100 qpc_bracket=1\n"
                + "OWNED_D3D12_QUERY_COPY_PASS rounds=8 separate_execute=1 same_slot_reuse=1 "
                  "duplicate_resolve=1 guards=1 payload_bytes=524288 inter_submission_cpu_waits=0 "
                  "final_fence=1 release_before_completion=1\n"
                + "".join(f"OWNED_D3D12_QUERY_COPY_VALUES round={i} pre={pre} post={post} "
                          f"successor={successor} duplicate={duplicate} reused={reused} guards=1\n"
                          for i, (pre, post, successor, duplicate, reused) in enumerate(self.copy_values)))

    def test_ordered_samples_and_equal_ticks_are_valid(self):
        self.values[1] = (102, 102, 102)
        self.copy_values[1] = (103, 103, 103, 103, 103)
        self.assertEqual(runner.query_ordering_failures(self.trace()), [])

    def test_copy_resolve_ordering_and_calibration_domain_are_enforced(self):
        for values in ((108, 107, 110, 107, 111), (106, 109, 110, 109, 111),
                       (108, 109, 110, 108, 111), (108, 109, 110, 109, 141)):
            with self.subTest(values=values):
                self.copy_values[2] = values
                self.assertTrue(runner.query_ordering_failures(self.trace()))

    def test_copy_missing_round_guards_and_calibration_are_rejected(self):
        trace = self.trace()
        for old, new in (("gpu_before=90", "gpu_before=101"),
                         ("frequency=24000000", "frequency=0"),
                         ("guards=1", "guards=0"),
                         ("OWNED_D3D12_QUERY_COPY_CALIBRATION", "MISSING_CALIBRATION")):
            with self.subTest(old=old):
                self.assertTrue(runner.query_ordering_failures(trace.replace(old, new)))
        self.copy_values.pop()
        self.assertTrue(runner.query_ordering_failures(self.trace()))

    def test_missing_round_and_missing_scenario_are_rejected(self):
        trace = self.trace().split("\n", 1)[1]
        self.assertTrue(runner.query_ordering_failures(trace))
        self.values.pop()
        self.assertTrue(runner.query_ordering_failures(self.trace()))

    def test_successor_and_reused_slot_cannot_precede_previous_work(self):
        for values in ((106, 105, 108), (106, 107, 106), (104, 107, 108)):
            with self.subTest(values=values):
                self.values[2] = values
                self.assertTrue(runner.query_ordering_failures(self.trace()))

    def test_zero_and_metal_error_values_are_rejected(self):
        for values in ((0, 101, 102), (100, 101, 2**64 - 1)):
            with self.subTest(values=values):
                self.values[0] = values
                self.assertTrue(runner.query_ordering_failures(self.trace()))


class ReviewEvidenceFailures(unittest.TestCase):
    def simulate(self, suite, trace, *, native=True):
        with tempfile.TemporaryDirectory(dir=SCRATCH, prefix="review-") as directory:
            base = Path(directory)
            runtime = base / "runtime"
            (runtime / "lib/wine/x86_64-unix").mkdir(parents=True)
            (runtime / "owned-graphics-runtime.json").write_text('{"schema": 1}\n')
            (runtime / "lib/wine/x86_64-unix/winemetal.so").write_bytes(b"native")
            fixture = base / "controlled.exe"
            fixture.write_bytes(controlled_pe_fixture())
            dxil = base / "dxil"
            dxil.mkdir()
            for name in ("resource-review-cs_bounds.dxil", "resource-review-cs_gradients.dxil",
                         "resource-review-cs_array_atomics.dxil", "resource-review-cs_bounds64.dxil"):
                (dxil / name).write_bytes(b"controlled shader input")
            if suite == "writable-msaa":
                for entry in runner.WRITABLE_MSAA_ENTRIES:
                    (dxil / f"sm6-{entry}.dxil").write_bytes(b"controlled shader input")
            args = argparse.Namespace(runtime=runtime, lock=runtime / "owned-graphics-runtime.json",
                                      fixture=fixture, dxil_dir=dxil, out=base / "result",
                                      suite=suite, mode="no-present", validation=False,
                                      api_validation=False, timeout=1)

            def fake_run(command, **kwargs):
                if command[0] == str(runtime / "bin/wine"):
                    kwargs["stdout"].write(trace)
                    if native:
                        kwargs["stderr"].write(f"dyld[1]: {runtime}/lib/wine/x86_64-unix/winemetal.so\n")
                return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

            with patch.object(runner.subprocess, "run", side_effect=fake_run), \
                 contextlib.redirect_stdout(io.StringIO()):
                runner.run(args)
            return json.loads((args.out / "result.json").read_text())["pass"]

    def test_shader_review_rejects_missing_case_and_mismatched_readback(self):
        trace = "OWNED_D3D12_DXIL_RESOURCE_REVIEW_BACKEND id=owned-graphics-d3d12\n"
        for name, count in (("bounds", 26), ("gradients", 6), ("array-atomics", 24), ("bounds64", 17)):
            trace += "".join(f"OWNED_D3D12_DXIL_RESOURCE_REVIEW_WORD case={name} index={index} "
                             "actual=0x0000000000000000 expected=0x0000000000000000\n"
                             for index in range(count))
            trace += f"OWNED_D3D12_DXIL_RESOURCE_REVIEW_CASE_PASS case={name} checks={count}\n"
        trace += "OWNED_D3D12_DXIL_RESOURCE_REVIEW_PASS\n"
        self.assertTrue(self.simulate("dxil-resource-review", trace))
        self.assertFalse(self.simulate("dxil-resource-review", trace.replace(
            "OWNED_D3D12_DXIL_RESOURCE_REVIEW_CASE_PASS case=bounds64 checks=17\n", "")))
        self.assertFalse(self.simulate("dxil-resource-review", trace.replace(
            "OWNED_D3D12_DXIL_RESOURCE_REVIEW_WORD case=bounds64 index=16 "
            "actual=0x0000000000000000 expected=0x0000000000000000\n", "")))
        self.assertFalse(self.simulate("dxil-resource-review", trace.replace(
            "actual=0x0000000000000000", "actual=0x00000000ffffffff")))
        self.assertFalse(self.simulate("dxil-resource-review", trace, native=False))

    def test_writable_msaa_requires_every_shape_scalar_and_native_compiler(self):
        # Controlled runner-contract input only; this is not GPU execution evidence.
        cases = []
        for samples in (2, 4, 8):
            for array in (False, True):
                cases += ["same_dispatch", "next_dispatch_dimensions", "SRV_after_transition", "direct_heap",
                          "out_of_bounds_sample_xy_layer", "UAV_clear_barrier", "native_copy",
                          "native_raster", "native_resolve_min_max"]
                if array:
                    cases.append("array_subview")
                cases.append("null_store_load")
        for scalar in ("sint", "float"):
            cases += [f"{scalar}_{case}" for case in ("same_dispatch", "next_dispatch_dimensions",
                      "SRV_after_transition", "native_copy", "native_raster", "native_resolve_min_max", "UAV_clear")]
        cases += ["typeless16_same_dispatch", "typeless16_next_dispatch", "typeless16_native_copy_float_srv"]
        trace = ("OWNED_D3D12_WRITABLE_MSAA_BACKEND id=owned-graphics-d3d12\n"
                 "OWNED_D3D12_WRITABLE_MSAA_CAPS writable=1\n")
        trace += "".join(f"OWNED_D3D12_SM6_CASE_PASS {case}\n" for case in cases)
        trace += "".join(f"OWNED_D3D12_WRITABLE_MSAA_SHAPE samples={samples} array={array}\n"
                         for samples in (2, 4, 8) for array in (0, 1))
        trace += ("OWNED_D3D12_WRITABLE_MSAA_SCALAR type=sint\n"
                  "OWNED_D3D12_WRITABLE_MSAA_SCALAR type=float\nOWNED_D3D12_WRITABLE_MSAA_PASS\n")
        self.assertTrue(self.simulate("writable-msaa", trace))
        self.assertFalse(self.simulate("writable-msaa", trace, native=False))
        for line in ("OWNED_D3D12_SM6_CASE_PASS out_of_bounds_sample_xy_layer\n",
                     "OWNED_D3D12_WRITABLE_MSAA_CAPS writable=1\n",
                     "OWNED_D3D12_SM6_CASE_PASS float_native_resolve_min_max\n",
                     "OWNED_D3D12_SM6_CASE_PASS typeless16_native_copy_float_srv\n",
                     "OWNED_D3D12_WRITABLE_MSAA_SHAPE samples=8 array=1\n",
                     "OWNED_D3D12_WRITABLE_MSAA_SCALAR type=float\n"):
            with self.subTest(line=line):
                self.assertFalse(self.simulate("writable-msaa", trace.replace(line, "", 1)))
        for extra in ("OWNED_D3D12_SM6_CASE_PASS same_dispatch\n",
                      "OWNED_D3D12_SM6_MISMATCH invalid samples\n",
                      "OWNED_D3D12_WRITABLE_MSAA_PASS\n"):
            with self.subTest(extra=extra):
                self.assertFalse(self.simulate("writable-msaa", trace + extra))

    def test_mapping_requires_live_control_view_and_no_growth(self):
        trace = ("OWNED_D3D12_ARENA_HEAP_BACKEND id=owned-graphics-d3d12\n"
                 "OWNED_D3D12_ARENA_HEAP_MAPPING_PASS failures=32 views_before=1 views_after=1\n")
        self.assertTrue(self.simulate("arena-heap-mapping", trace))
        self.assertFalse(self.simulate("arena-heap-mapping", trace.replace("views_after=1", "views_after=33")))
        self.assertFalse(self.simulate("arena-heap-mapping", trace.replace("views_before=1 views_after=1",
                                                                          "views_before=0 views_after=0")))


class FsrPresentationEvidence(unittest.TestCase):
    def test_multi_phase_native_pixels_and_zero_output_suppression(self):
        # CPU-only parser protocol, deliberately NOT GPU/native display proof.
        spec = importlib.util.spec_from_file_location("fsr_capture_checker",
            ROOT / "graphics-translator/tests/check-owned-fg-capture.py")
        checker = importlib.util.module_from_spec(spec); spec.loader.exec_module(checker)
        with tempfile.TemporaryDirectory(dir=SCRATCH, prefix="fsr-present-") as directory:
            base = Path(directory); fixture = base / "fixture"; captures = base / "fg-capture"
            fixture.mkdir(); captures.mkdir()
            stdout = "OWNED_D3D12_FSR_FG_PASS\n"
            stderr = "OWNED_D3D12_FSR_FG_OBSERVER_READY\n"
            expected = [(1, 0, 0), (2, 1, 1), (2, 0, 0), (3, 1, 1), (3, 1, 2), (3, 0, 0),
                        (4, 1, 1), (4, 1, 2), (4, 1, 3), (4, 0, 0), (5, 0, 0), (6, 0, 0), (7, 0, 0),
                        (8, 1, 1), (8, 1, 2), (8, 1, 3), (8, 1, 4), (9, 0, 0), (11, 0, 0)]
            for sequence, (frame, generated, index) in enumerate(expected, 1):
                image = bytearray(bytes((32 + index * 16, 60, 100, 255)) * (128 * 128))
                marker = bytes((frame + 32, 190 if generated else 70, 220, 255))
                for y in range(8): image[y * 128 * 4:y * 128 * 4 + 32] = marker * 8
                (fixture / f"fg-callback-{frame}-{generated}-{index if generated else 1}.bin").write_bytes(image)
                (captures / f"drawable-{sequence}.bin").write_bytes(image)
                stdout += f"OWNED_D3D12_FSR_FG_CALLBACK_PIXELS frame={frame} generated={generated} output_index={index} width=128 height=128\n"
                stderr += (f"OWNED_D3D12_FSR_FG_DRAWABLE sequence={sequence} drawable=d{sequence} width=128 height=128 format=70 path=drawable-{sequence}.bin\n"
                           f"OWNED_D3D12_FSR_FG_PRESENTED sequence={sequence} drawable=d{sequence} time={sequence}\n")
            for sequence, frame in enumerate(range(11, 15), len(expected) + 1):
                body = bytes((frame * 8, 60, 100, 255))
                overlay = bytes((round(64 + frame * 8 * 127 / 255), round(60 * 127 / 255), round(100 * 127 / 255), 255))
                image = bytearray(body * (128 * 128))
                for y in range(8): image[y * 128 * 4:y * 128 * 4 + 32] = overlay * 8
                (captures / f"drawable-{sequence}.bin").write_bytes(image)
                stdout += f"OWNED_D3D12_FSR_FG_UI_FRAME frame={frame}\n"
                stderr += (f"OWNED_D3D12_FSR_FG_DRAWABLE sequence={sequence} drawable=d{sequence} width=128 height=128 format=70 path=drawable-{sequence}.bin\n"
                           f"OWNED_D3D12_FSR_FG_PRESENTED sequence={sequence} drawable=d{sequence} time={sequence}\n")
            for stream, frames in (("callback", [*range(1, 10), 11, 12]), ("builtin_ui", list(range(11, 15)))):
                for frame in frames:
                    submitted = sum(f <= frame for f, _, _ in expected) if stream == "callback" else frame - 10
                    unmeasured = frame == frames[0]
                    measured = 0 if unmeasured else max(1, submitted - 1)
                    refresh = 0 if unmeasured else submitted
                    stdout += (f"OWNED_D3D12_FSR_FG_STATISTICS frame={frame} statistics={'disjoint' if unmeasured else 'actual'} "
                               f"result={'0x887a000b' if unmeasured else '0x00000000'} submitted_present={submitted} native_present={measured} "
                               f"present_refresh={refresh} sync_refresh={refresh} sync_qpc={measured * 100} qpc_end=20000 frequency=1000 source=public_dxgi_actual_presentation\n")
                stdout += (f"OWNED_D3D12_FSR_FG_FINAL_STATISTICS stream={stream} output_vblanks=3 statistics=actual "
                           f"result=0x00000000 submitted_present={submitted} native_present={measured} present_refresh={refresh} "
                           f"sync_refresh={refresh} sync_qpc={measured * 100} qpc_end=20000 frequency=1000 source=public_dxgi_actual_presentation\n")
            args = argparse.Namespace(stdout=base / "stdout.log", stderr=base / "stderr.log", captures=captures, native=None, suite="fsr-fg")
            args.stdout.write_text(stdout); args.stderr.write_text(stderr)
            self.assertEqual(checker.check(args)["attempted_drawables"], 23)
            args.stdout.write_text(stdout.replace("frame=1 generated=0 output_index=0", "frame=1 generated=1 output_index=1"))
            with self.assertRaisesRegex(ValueError, "sequence mismatch"): checker.check(args)
            args.stdout.write_text(stdout)
            bad = bytearray((fixture / "fg-callback-3-1-2.bin").read_bytes())
            bad[128 * 20 * 4:128 * 21 * 4] = bytes((19, 19, 19, 255)) * 128
            (captures / "drawable-5.bin").write_bytes(bad)
            with self.assertRaisesRegex(ValueError, "content mismatch"): checker.check(args)
            (captures / "drawable-5.bin").write_bytes((fixture / "fg-callback-3-1-2.bin").read_bytes())
            args.stderr.write_text(stderr.replace("sequence=5 drawable=d5 time=5", "sequence=5 drawable=d5 time=0"))
            with self.assertRaisesRegex(ValueError, "physical phase proof incomplete"): checker.check(args)
            args.stderr.write_text(stderr.replace("sequence=1 drawable=d1 time=1", "sequence=1 drawable=d1 time=0"))
            # A baseline skip is explicit, but every N=1..4 generated index,
            # critical real/error output and UI proof remains exercised.
            result = checker.check(args)
            self.assertEqual(result["positively_displayed_drawables"], 22)
            self.assertEqual(result["skipped_drawables"], 1)
            self.assertEqual(len(result["physically_verified_generated_phases"]), 10)
            args.stderr.write_text(stderr)
            duplicate = (fixture / "fg-callback-3-1-1.bin").read_bytes()
            (fixture / "fg-callback-3-1-2.bin").write_bytes(duplicate)
            (captures / "drawable-5.bin").write_bytes(duplicate)
            with self.assertRaisesRegex(ValueError, "repeats another phase"): checker.check(args)


class NgxPresentationEvidence(unittest.TestCase):
    def test_latest_statistics_are_counts_not_submitted_ids(self):
        spec = importlib.util.spec_from_file_location("fg_statistics_checker",
            ROOT / "graphics-translator/tests/check-owned-fg-capture.py")
        checker = importlib.util.module_from_spec(spec); spec.loader.exec_module(checker)
        measured = {"submitted_present": "8", "frequency": "1000", "statistics": "actual", "result": "0x00000000",
                    "actual_present": "3", "sync_qpc": "100", "present_refresh": "4", "sync_refresh": "5", "qpc_end": "200"}
        disjoint = dict(measured, statistics="disjoint", result="0x887a000b", actual_present="0", sync_qpc="0", present_refresh="0", sync_refresh="0")
        rows = [measured, dict(measured, submitted_present="9"), disjoint,
                dict(measured, submitted_present="10", actual_present="4", sync_qpc="110", present_refresh="5", sync_refresh="6")]
        observations, previous = checker.statistics_observations(rows, "actual_present")
        self.assertEqual([row["displayed_count"] for row in observations], [3, 3, 0, 4])
        self.assertEqual(previous, (4, 110, 5, 6))
        for broken, reason in ((dict(measured, actual_present="9"), "count/refresh/clock/result"),
                               (dict(measured, actual_present="0"), "count/refresh/clock/result"),
                               (dict(measured, present_refresh="6"), "count/refresh/clock/result"),
                               (dict(measured, sync_qpc="201"), "future"),
                               (dict(disjoint, actual_present="1"), "disjoint"),
                               (dict(disjoint, result="0x00000000"), "disjoint")):
            with self.subTest(broken=broken), self.assertRaisesRegex(ValueError, reason):
                checker.statistics_observations([broken], "actual_present")
        with self.assertRaisesRegex(ValueError, "regress"):
            checker.statistics_observations([measured, disjoint, dict(measured, actual_present="2")], "actual_present")

    def test_native_pixels_display_and_order_are_required_not_summary(self):
        # CPU-only parser protocol, deliberately NOT GPU/native display proof.
        spec = importlib.util.spec_from_file_location("fg_capture_checker",
            ROOT / "graphics-translator/tests/check-owned-fg-capture.py")
        checker = importlib.util.module_from_spec(spec); spec.loader.exec_module(checker)
        with tempfile.TemporaryDirectory(dir=SCRATCH, prefix="ngx-present-") as directory:
            base = Path(directory); fixture = base / "fixture"; captures = base / "fg-capture"
            fixture.mkdir(); captures.mkdir()
            prefix = "OWNED_D3D12_NGX_PRESENT"
            summary = prefix + "_PASS frames=6 drawables=9 generated=3 suppressed=3 ngx_feature=11 hudless=1 ui=current_backbuffer history=fresh_diff reset=untouched release_before_execute=1 replay=exact caller_owned_present=1\n"
            stdout, stderr = summary, prefix + "_OBSERVER_READY public_selectors=1\n"
            args = argparse.Namespace(stdout=base / "stdout.log", stderr=base / "stderr.log",
                                      captures=captures, suite="ngx-fg-present")
            args.stdout.write_text(stdout); args.stderr.write_text(stderr)
            with self.assertRaisesRegex(ValueError, "sequence incomplete"):
                checker.check(args)
            for attempt in (1, 2):
                pixels = bytes((16 + attempt, 80, 176, 255)) * (128 * 128)
                (fixture / f"fg-ngx-bootstrap-{attempt}.bin").write_bytes(pixels)
                (captures / f"drawable-{attempt:06}.bin").write_bytes(pixels)
                stdout += f"{prefix}_BOOTSTRAP attempt={attempt} width=128 height=128 submitted_present={attempt} rgb={16 + attempt},80,176 source=caller_real_gpu_copy_to_dxgi\n"
                stderr += f"{prefix}_DRAWABLE sequence={attempt} drawable={attempt} width=128 height=128 format=70 path=drawable-{attempt:06}.bin\n"
                stderr += f"{prefix}_PRESENTED sequence={attempt} drawable={attempt} time={0 if attempt == 1 else 1 + attempt / 60:.9f}\n"
            stdout += f"{prefix}_BOOTSTRAP_READY attempt=2 submitted_present=2 statistics=actual result=0x00000000 actual_present=1 sync_qpc=100 present_refresh=1 sync_refresh=2 qpc_end=250 frequency=1000 source=public_dxgi_actual_presentation\n"
            sentinel = bytes((19,)) * (128 * 128 * 4)
            sequence = 2
            for frame in range(1, 7):
                pixels = bytearray()
                for y in range(128):
                    for x in range(128):
                        on = (((x + frame * 4) // 16) ^ (y // 16)) & 1
                        rgb = (128 if on else 64,
                               200 if 40 + frame * 2 < x < 64 + frame * 2 and 40 < y < 96 else 32,
                               32 if on else 128)
                        pixels.extend((*((32 + frame * 24, 220, 48) if x < 32 and y < 32 else rgb), 255))
                for generated in ((1, 0) if frame in (2, 3, 4) else (0,)):
                    sequence += 1
                    image = bytearray(pixels)
                    if generated: image[-4] ^= 1  # Non-probe synthetic distinction, not GPU interpolation proof.
                    (fixture / f"fg-ngx-{frame}-{generated}.bin").write_bytes(image)
                    (captures / f"drawable-{sequence:06}.bin").write_bytes(image)
                    stdout += f"{prefix}_FRAME frame={frame} generated={generated} phase={.5 if generated else 1.} actual=1 output_flag=0 width=128 height=128 submitted_present={sequence} statistics=actual result=0x00000000 actual_present={sequence - 1} sync_qpc={sequence * 100} present_refresh={sequence} sync_refresh={sequence} pace_qpc_start={sequence * 100 - 10} qpc_start={sequence * 100} qpc_end={sequence * 100 + 10} frequency=1000 source=ngx11_gpu_copy_to_dxgi\n"
                    stderr += f"{prefix}_DRAWABLE sequence={sequence} drawable={sequence} width=128 height=128 format=70 path=drawable-{sequence:06}.bin\n"
                    stderr += f"{prefix}_PRESENTED sequence={sequence} drawable={sequence} time={1 + sequence / 60:.9f}\n"
                if frame in (1, 5, 6):
                    (fixture / f"fg-ngx-{frame}-1.bin").write_bytes(sentinel)
                    stdout += f"OWNED_D3D12_NGX_SUPPRESSED frame={frame} actual=0 flag=1 output_untouched=1\n"
                if frame in (4, 5): (fixture / f"fg-ngx-fresh-{frame}.bin").write_bytes(sentinel)
                if frame == 6: (fixture / "fg-ngx-replay-6.bin").write_bytes(sentinel)
            stdout += f"{prefix}_DRAIN submitted_present=11 statistics=actual result=0x00000000 actual_present=10 sync_qpc=1100 present_refresh=11 sync_refresh=11 qpc_end=1150 frequency=1000 output_vblanks=3\n"
            args.stdout.write_text(stdout); args.stderr.write_text(stderr)
            result = checker.check(args)
            self.assertTrue(result["pass"])
            self.assertEqual(result["attempted_drawables"], 9)
            self.assertEqual(result["positively_displayed_drawables"], 9)
            self.assertEqual(len(result["suppressed_generated_outputs"]), 3)
            unmeasured = stdout.replace("statistics=actual result=0x00000000 actual_present=2 sync_qpc=300 present_refresh=3 sync_refresh=3",
                                        "statistics=disjoint result=0x887a000b actual_present=0 sync_qpc=0 present_refresh=0 sync_refresh=0", 1)
            args.stdout.write_text(unmeasured)
            self.assertTrue(checker.check(args)["pass"])
            args.stdout.write_text(stdout)
            args.stderr.write_text(stderr.replace("time=1.050000000", "time=0"))
            self.assertEqual(checker.check(args)["positively_displayed_drawables"], 8)
            args.stderr.write_text(stderr.replace("time=0.000000000", "time=1.010000000", 1).replace("time=1.033333333", "time=0"))
            self.assertTrue(checker.check(args)["pass"])  # Latest ready sample may describe an earlier bootstrap display.
            equal_bootstrap = stderr.replace("time=0.000000000", "time=1.033333333", 1)
            args.stderr.write_text(equal_bootstrap)
            self.assertTrue(checker.check(args)["pass"])  # Bootstrap handlers may share one positive native clock sample.
            args.stderr.write_text(equal_bootstrap.replace("time=1.033333333", "time=1.040000000", 1))
            with self.assertRaisesRegex(ValueError, "bootstrap actual display timestamps reordered"):
                checker.check(args)
            for broken, reason in ((stderr.replace("time=1.066666667", "time=0"), "critical generated phase"),
                                   (stderr.replace("time=1.100000000", "time=0"), "critical generated phase"),
                                   (stderr.replace("time=1.133333333", "time=0"), "critical generated phase"),
                                   (stderr.replace("time=1.083333333", "time=0"), "critical real frame lacks actual"),
                                   (stderr.replace("time=1.083333333", "time=1.04"), "paced ordered"),
                                   (stderr.replace("time=1.033333333", "time=0"), "visibility-ready")):
                args.stderr.write_text(broken)
                with self.assertRaisesRegex(ValueError, reason): checker.check(args)
            args.stderr.write_text(stderr)
            for broken, reason in ((stdout.replace("phase=0.5", "phase=0.6", 1), "source phase/status"),
                                   (stdout.replace("actual=0 flag=1", "actual=1 flag=0", 1), "flag/suppression"),
                                   (stdout + summary, "consumer incomplete")):
                args.stdout.write_text(broken)
                with self.assertRaisesRegex(ValueError, reason): checker.check(args)
            args.stdout.write_text(stdout.replace(summary, prefix + "_PASS caller_owned_present=1 replay=exact release_before_execute=1 hudless=1 ngx_feature=11 suppressed=3 generated=3 drawables=9 frames=6\n"))
            self.assertTrue(checker.check(args)["pass"])
            args.stdout.write_text(stdout)
            boot = captures / "drawable-000001.bin"; original = boot.read_bytes()
            boot.write_bytes(bytes((0, 0, 0, 255)) * (128 * 128))
            with self.assertRaisesRegex(ValueError, "bootstrap native drawable pixels"): checker.check(args)
            boot.write_bytes(original)
            path = captures / "drawable-000003.bin"; original = path.read_bytes()
            corrupt = bytearray(original); corrupt[(4 * 128 + 4) * 4] = 0; path.write_bytes(corrupt)
            with self.assertRaisesRegex(ValueError, "drawable content mismatch"): checker.check(args)
            path.write_bytes(original)
            public = fixture / "fg-ngx-1-1.bin"; public.write_bytes(bytes((0,)) * len(sentinel))
            with self.assertRaisesRegex(ValueError, "suppressed actual0 output changed"): checker.check(args)
            public.write_bytes(sentinel)
            public = fixture / "fg-ngx-2-1.bin"; original = public.read_bytes()
            public.write_bytes((fixture / "fg-ngx-2-0.bin").read_bytes())
            with self.assertRaisesRegex(ValueError, "generated phase repeats real frame"): checker.check(args)
            public.write_bytes(original)
            corrupt = bytearray(public.read_bytes()); corrupt[(4 * 128 + 4) * 4] = 0; public.write_bytes(corrupt)
            with self.assertRaisesRegex(ValueError, "independent scene/HUD"): checker.check(args)


class GameSuiteEvidence(unittest.TestCase):
    """CPU-only synthetic protocols, not observed GPU evidence."""
    def setUp(self):
        self.alpha = "".join(f"OWNED_D3D12_SR_ALPHA_CASE_PASS frame={f} replay={r} scalar_oracle_max_half_ulp=2 subrect=1 dynamic={int(f == 3)} retired={int(f == 5)}\n" for f in range(6) for r in range(2 if f == 5 else 1)) + "OWNED_D3D12_SR_ALPHA_PASS frames=6 replay_after_release=1 independent_scalar_oracle=1 history_delta=42\n"
        self.debug = "".join(f"OWNED_D3D12_FSR_DEBUG_CASE_PASS extent={e} replay={r} panel_max_byte_error=1 sentinel=1 retired={int(e == 96)}\n" for e in (128, 96) for r in (0, 1)) + "OWNED_D3D12_FSR_DEBUG_PASS public_dispatch=1 depth_gradient=1 signed_motion=1 color=1 dynamic=1 retirement_replay=1\n"
        self.ngx = ("OWNED_D3D12_NGX_FEATURE_CASE_PASS case=exact_capabilities fg=1 rr=1 non_sr_other=0\n"
                    "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_fg count_max=4 per_index_output=1 grouped_recording=1 actual0_untouched=1 immutable_params=1 release_before_execute=1 replay=1\n")
        self.ngx += "".join(f"OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_rr variant={v} frames=8 handle13=1 denoising_guides=1 matrices_snapshotted=1 release_before_execute=1 replay=1 oracle_artifacts=8\n" for v in range(4))
        self.ngx += "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_alpha flag128=1 nonopaque_contrast=1 rgb_preserved=1\nOWNED_D3D12_NGX_FEATURES_PASS\n"
        ngx_phases = ""
        for grouped in (0, 1):
            for count in range(1, 5):
                for frame in range(7):
                    for index in range(1, count + 1):
                        disabled = frame in (0, 2, 4, 6)
                        phase = index / (count + 1)
                        red = 19 if disabled else round(32 + (frame - 1) * 8 + phase * 8)
                        ngx_phases += f"OWNED_D3D12_NGX_PHASE frame={frame} count={count} index={index} phase={phase:g} actual={int(not disabled)} grouped={grouped} actual_count={0 if disabled else count} output_flag={int(disabled)} untouched={int(disabled)} probe_red={red} consumer=gpu_readback\n"
        ngx_scenes = "".join(f"OWNED_D3D12_NGX_SCENE count={count} actual=0 output_flag=1 untouched=1 phases={count} reset=0 consumer=gpu_readback\n" for count in range(1, 5))
        self.ngx = self.ngx.replace("OWNED_D3D12_NGX_FEATURES_PASS\n", ngx_phases + ngx_scenes + "OWNED_D3D12_NGX_FEATURES_PASS\n")
        self.ngx_fg = "".join(line for line in self.ngx.splitlines(keepends=True) if
            line.startswith(("OWNED_D3D12_NGX_PHASE ", "OWNED_D3D12_NGX_SCENE ",
                "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=exact_capabilities ", "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_fg ")))
        self.ngx_fg += "OWNED_D3D12_NGX_FG_PASS capacity=4 independent=1 grouped=1\n"
        self.ngx_alpha = ("OWNED_D3D12_NGX_ALPHA_PIXELS replay=0 left=112,96,80,48 right=112,96,80,192 rgb_max_error=0 release_before_execute=1\n"
                          "OWNED_D3D12_NGX_ALPHA_PIXELS replay=1 left=112,96,80,48 right=112,96,80,192 rgb_max_error=0 release_before_execute=1\n"
                          "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_alpha flag128=1 nonopaque_contrast=1 rgb_preserved=1\n"
                          "OWNED_D3D12_NGX_ALPHA_PASS cases=1 release_before_execute=1 replay=1 rgb_alpha=gpu_readback\n")
        self.factory = "".join(f"DXGI_FACTORY_ADAPTER preference={p} index=0 luid=00000001:00000002\n" for p in range(3)) + "DXGI_FACTORY_EVENTS_OK identity preference nodes occlusion stereo lifetime\n"
        self.fullscreen = """OWNED_DXGI_FULLSCREEN_FRAME api=d3d12 pixels=3888 exact=1 fence=complete present=ok
OWNED_DXGI_FULLSCREEN_TRANSITION api=d3d12 enter=1 resize=1 frame=1 leave=1 rect=restored
OWNED_DXGI_FULLSCREEN_REPEAT api=d3d12 cycles=2 restored=1
OWNED_DXGI_FULLSCREEN_ERRORS api=d3d12 invalid=1 qi=1 desc=1 mode=1 resize=1 reentrant=1 native_enter=1 native_leave=1 preserved=1
OWNED_DXGI_FULLSCREEN_CREATION api=d3d12 native=1 windowed=1 rect=restored
OWNED_DXGI_FULLSCREEN_INVALID_CREATION api=d3d12 rejected=1 null_output=1
OWNED_DXGI_OUTPUT_VBLANK waits=3 elapsed_qpc=100 frequency=1000
OWNED_DXGI_OUTPUT_STATISTICS present=2 present_refresh=3 sync_refresh=3 qpc=120
OWNED_DXGI_OUTPUT_STATISTICS_PASS native=1 resize_disjoint=1 leave_unregistered=1
OWNED_DXGI_FULLSCREEN_PASS
"""

    def test_missing_duplicate_and_failed_protocols(self):
        for suite, trace in (("sr-alpha", self.alpha), ("fsr-debug", self.debug), ("ngx-features", self.ngx),
                             ("ngx-alpha", self.ngx_alpha), ("ngx-fg", self.ngx_fg), ("dxgi-factory", self.factory), ("dxgi-fullscreen", self.fullscreen)):
            self.assertEqual(runner.game_suite_failures(suite, trace), [])
            lines = trace.splitlines()
            for index, line in enumerate(lines):
                with self.subTest(suite=suite, line=line):
                    self.assertTrue(runner.game_suite_failures(suite, "\n".join(lines[:index] + lines[index + 1:])))
                    self.assertTrue(runner.game_suite_failures(suite, trace + line + "\n"))
            self.assertTrue(runner.game_suite_failures(suite, trace, "OWNED_FIXTURE_FAIL"))

    def test_numerical_and_lifetime_mutations(self):
        for suite, trace, before, after in (
            ("sr-alpha", self.alpha, "half_ulp=2", "half_ulp=5"),
            ("sr-alpha", self.alpha, "history_delta=42", "history_delta=0"),
            ("sr-alpha", self.alpha, "retired=1", "retired=0"),
            ("fsr-debug", self.debug, "byte_error=1", "byte_error=3"),
            ("fsr-debug", self.debug, "sentinel=1", "sentinel=0"),
            ("ngx-features", self.ngx, "variant=3", "variant=2"),
            ("ngx-features", self.ngx, "matrices_snapshotted=1", "matrices_snapshotted=0"),
            ("ngx-fg", self.ngx_fg, "phase=0.5 actual=1", "phase=0.4 actual=1"),
            ("ngx-fg", self.ngx_fg, "actual_count=0 output_flag=1", "actual_count=1 output_flag=1"),
            ("ngx-fg", self.ngx_fg, "actual_count=4 output_flag=0", "actual_count=1 output_flag=0"),
            ("ngx-fg", self.ngx_fg, "untouched=1 probe_red=19", "untouched=0 probe_red=19"),
            ("ngx-fg", self.ngx_fg, "probe_red=36", "probe_red=40"),
            ("ngx-fg", self.ngx_fg, "phases=4 reset=0", "phases=4 reset=1"),
            ("ngx-fg", self.ngx_fg, "consumer=gpu_readback", "consumer=wiring_copy"),
            ("ngx-alpha", self.ngx_alpha, "right=112,96,80,192", "right=112,96,80,255"),
            ("ngx-alpha", self.ngx_alpha, "left=112,96,80,48", "left=112,112,80,48"),
            ("ngx-alpha", self.ngx_alpha, "rgb_max_error=0", "rgb_max_error=8"),
            ("ngx-alpha", self.ngx_alpha, "release_before_execute=1", "release_before_execute=0"),
            ("dxgi-factory", self.factory, "preference=2", "preference=1"),
            ("dxgi-factory", self.factory, "index=0", "index=1"),
            ("dxgi-fullscreen", self.fullscreen, "qpc=120", "qpc=0"),
            ("dxgi-fullscreen", self.fullscreen, "resize_disjoint=1", "resize_disjoint=0")):
            self.assertTrue(runner.game_suite_failures(suite, trace.replace(before, after, 1)))

    def test_ngx_fg_cannot_replace_full_rr_alpha_or_phase_evidence(self):
        # Controlled CPU parser records, never GPU proof.
        self.assertEqual(runner.game_suite_failures("ngx-fg", self.ngx_fg), [])
        for suite, trace in (("ngx-features", self.ngx_fg), ("ngx-fg", self.ngx),
                ("ngx-fg", self.ngx_alpha), ("ngx-fg", self.alpha)):
            self.assertTrue(runner.game_suite_failures(suite, trace))
        self.assertTrue(runner.game_suite_failures("ngx-fg", self.ngx_fg + "OWNED_D3D12_NGX_FEATURES_PASS\n"))
        self.assertTrue(runner.game_suite_failures("ngx-fg", self.ngx_fg + "OWNED_D3D12_NGX_FG_PASS capacity=1 independent=1 grouped=1\n"))
        self.assertTrue(runner.game_suite_failures("ngx-fg", self.ngx_fg, "OWNED_D3D12_TEMPORAL_FAIL device removed\n"))
        lines = self.ngx_fg.splitlines()
        indices = [index for index, line in enumerate(lines) if line.startswith("OWNED_D3D12_NGX_PHASE ")]
        lines[indices[0]], lines[indices[1]] = lines[indices[1]], lines[indices[0]]
        self.assertTrue(runner.game_suite_failures("ngx-fg", "\n".join(lines)))

    def test_ngx_alpha_cannot_replace_full_rr_or_private_alpha_evidence(self):
        # Controlled CPU parser records, never GPU proof.
        self.assertEqual(runner.game_suite_failures("ngx-alpha", self.ngx_alpha), [])
        self.assertTrue(runner.game_suite_failures("ngx-features", self.ngx_alpha))
        self.assertTrue(runner.game_suite_failures("ngx-alpha", self.ngx))
        self.assertTrue(runner.game_suite_failures("ngx-alpha", self.alpha))
        self.assertTrue(runner.game_suite_failures("ngx-alpha", self.ngx_alpha +
            "OWNED_D3D12_NGX_ALPHA_PASS cases=0\n"))
        self.assertTrue(runner.game_suite_failures("ngx-alpha", self.ngx_alpha,
            "OWNED_D3D12_TEMPORAL_FAIL step=GPU completion device removed\n"))

    def test_runner_native_backend_foreign_validation_gates(self):
        for native, marker, foreign, diagnostic in ((True, True, False, ""), (False, True, False, ""),
                (True, False, False, ""), (True, True, True, ""), (True, True, False, "Invalid Metal usage:")):
            with tempfile.TemporaryDirectory(dir=SCRATCH) as directory:
                base = Path(directory); runtime = base / "runtime"
                (runtime / "lib/wine/x86_64-unix").mkdir(parents=True)
                (runtime / "owned-graphics-runtime.json").write_text('{"schema":1}')
                if native:
                    (runtime / "lib/wine/x86_64-unix/winemetal.so").write_bytes(b"native")
                fixture = base / "alpha.exe"; fixture.write_bytes(controlled_pe_fixture())
                args = argparse.Namespace(runtime=runtime, fixture=fixture, lock=runtime / "owned-graphics-runtime.json",
                    dxil_dir=base, out=base / "result", suite="sr-alpha", mode="no-present", validation=True,
                    api_validation=False, timeout=1)
                def fake_run(command, **kwargs):
                    if command[0] == str(runtime / "bin/wine"):
                        kwargs["stdout"].write(self.alpha + ("OWNED_D3D12_TEMPORAL_BACKEND id=owned-graphics-d3d12\n" if marker else ""))
                        kwargs["stderr"].write(f"dyld[1]: {runtime}/lib/wine/x86_64-unix/winemetal.so\n" + diagnostic)
                        if foreign:
                            kwargs["stderr"].write("dyld[1]: /foreign/D3DMetal.framework/D3DMetal\n")
                    return subprocess.CompletedProcess(command, 0, stdout="", stderr="")
                with patch.object(runner.subprocess, "run", side_effect=fake_run), contextlib.redirect_stdout(io.StringIO()):
                    runner.run(args)
                report = json.loads((args.out / "result.json").read_text())
                self.assertEqual(report["pass"], native and marker and not foreign and not diagnostic)

    def test_fsr_debug_requires_selected_builtin_and_native_backend(self):
        trace = "OWNED_D3D12_TEMPORAL_BACKEND id=owned-graphics-d3d12\n" + self.debug
        simulate = RunnerFailures().simulate_metalfx
        self.assertTrue(simulate("fsr-debug", trace)["pass"])
        self.assertFalse(simulate("fsr-debug", trace, builtin=False)["pass"])
        self.assertFalse(simulate("fsr-debug", trace, native=False)["pass"])
        with self.assertRaisesRegex(ValueError, "fsr=true"):
            simulate("fsr-debug", trace, selected=False)

    def test_rr_history_reset_mirror_and_finite_rgb(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as directory:
            base = Path(directory); public = base / "public"; oracle = base / "oracle"
            public.mkdir(); oracle.mkdir()
            def image(value):
                return struct.pack("<4e", value, value, value, 1) * (128 * 128)
            for frame in range(1, 9):
                for prefix, value in (("native-rr-frame", frame / 16), ("native-rr-transposed", frame / 16),
                        ("native-rr-mirror", frame / 32), ("native-rr-mirror-transposed", frame / 32 + .25)):
                    (oracle / f"{prefix}-{frame}.bin").write_bytes(image(value))
            (oracle / "native-rr-replay-8.bin").write_bytes(image(.75))
            (oracle / "native-rr-mirror-replay-8.bin").write_bytes(image(.875))
            for variant in ("", "linear-", "packed-", "mirror-"):
                for frame in range(1, 10):
                    name = f"public-rr-{variant}{'replay' if frame == 9 else f'frame-{frame}'}.bin"
                    value = (.875 if variant == "mirror-" else .75) if frame == 9 else frame / (32 if variant == "mirror-" else 16)
                    (public / name).write_bytes(image(value))
            for variant in ("", "mirror-"):
                for frame in range(1, 9):
                    payload = bytes([frame]) * (128 * 128 * 48 + 140)
                    for root, prefix in ((public, "public"), (oracle, "native")):
                        (root / f"{prefix}-rr-{variant}input-{frame}.inputs").write_bytes(payload)
            report = runner.rr_oracle_comparison(public, oracle)
            self.assertEqual(report["failures"], [])
            self.assertEqual(len(report["metrics"]), 36)
            self.assertEqual(len(report["input_metrics"]), 16)
            self.assertTrue(all(row["max_abs_error"] == row["mean_abs_error"] == 0 for row in report["metrics"]))
            runtime = base / "runtime"
            (runtime / "lib/wine/x86_64-unix").mkdir(parents=True)
            (runtime / "lib/wine/x86_64-windows").mkdir()
            (runtime / "lib/wine/x86_64-unix/winemetal.so").write_bytes(b"synthetic native")
            (runtime / "lib/wine/x86_64-windows/nvngx.dll").write_bytes(b"synthetic builtin")
            (runtime / "owned-graphics-runtime.json").write_text('{"schema":1}')
            (oracle / "owned-rr-native-control").write_bytes(b"synthetic independent control")
            (oracle / "evidence.json").write_text(json.dumps({"observed_result":
                "OWNED_RR_NATIVE_PASS frames=8 matrix_sensitive_frames=8 source=public-metalfx-denoiser",
                "finite_non_source_copy_frames": 32}))
            (oracle / "replay.stdout.log").write_text("OWNED_RR_NATIVE_REPLAY_PASS frames=18 history_frames=8 replay_frame=8 replay_reset=0 source=public-metalfx-denoiser\n")
            (oracle / "replay.stderr.log").write_text("")
            (oracle / "stdout.log").write_text("independent control output\n")
            (oracle / "stderr.log").write_text("")
            fixture = base / "ngx.exe"; fixture.write_bytes(controlled_pe_fixture())
            args = argparse.Namespace(runtime=runtime, fixture=fixture, lock=runtime / "owned-graphics-runtime.json",
                dxil_dir=base, out=base / "result", suite="ngx-features", mode="no-present", validation=True,
                api_validation=False, timeout=1, rr_oracle_dir=oracle)
            def fake_run(command, **kwargs):
                if command[0] == str(runtime / "bin/wine"):
                    kwargs["stdout"].write("OWNED_D3D12_TEMPORAL_BACKEND id=owned-graphics-d3d12\n" + self.ngx)
                    kwargs["stderr"].write(f"dyld[1]: {runtime}/lib/wine/x86_64-unix/winemetal.so\n" +
                        'Loaded L"C:\\windows\\system32\\nvngx.dll" at 0001: builtin\n')
                    for path in public.iterdir():
                        (kwargs["cwd"] / path.name).write_bytes(path.read_bytes())
                return subprocess.CompletedProcess(command, 0, stdout="", stderr="")
            with patch.object(runner.subprocess, "run", side_effect=fake_run), contextlib.redirect_stdout(io.StringIO()):
                runner.run(args)
            self.assertTrue(json.loads((args.out / "result.json").read_text())["pass"])
            (oracle / "replay.stderr.log").write_text("Invalid Metal usage: native baseline fault\n")
            (oracle / "stderr.log").write_text("Shader Validation Error: original native fault\n")
            args.out = base / "native-fault-result"
            with patch.object(runner.subprocess, "run", side_effect=fake_run), contextlib.redirect_stdout(io.StringIO()):
                runner.run(args)
            failed_native_report = json.loads((args.out / "result.json").read_text())
            self.assertFalse(failed_native_report["pass"])
            self.assertEqual(len(failed_native_report["rr_oracle_comparison"]["metrics"]), 36)
            self.assertEqual(len(failed_native_report["rr_oracle_comparison"]["input_metrics"]), 16)
            self.assertEqual(len(failed_native_report["rr_native_validation_errors"]), 2)
            self.assertTrue(any("native baseline fault" in error for error in failed_native_report["validation_errors"]))
            input_target = public / "public-rr-input-1.inputs"
            original_inputs = input_target.read_bytes()
            input_target.write_bytes(bytes([2]) + original_inputs[1:])
            self.assertTrue(any("uploaded input bytes" in error for error in
                                runner.rr_oracle_comparison(public, oracle)["failures"]))
            input_target.write_bytes(original_inputs)
            target = public / "public-rr-frame-8.bin"; original = target.read_bytes()
            for payload in (image(.75), image(float("nan")), image(float("inf")), original[:-2]):
                target.write_bytes(payload)
                self.assertTrue(runner.rr_oracle_comparison(public, oracle)["failures"])
            target.unlink()
            self.assertTrue(runner.rr_oracle_comparison(public, oracle)["failures"])
            target.symlink_to(oracle / "native-rr-replay-8.bin")
            self.assertTrue(runner.rr_oracle_comparison(public, oracle)["failures"])


class ConservativeRasterEvidence(unittest.TestCase):
    def test_tier3_protocol_rejects_missing_duplicate_and_numeric_corruption(self):
        # Controlled parser input only, NOT GPU execution or frozen shader production evidence.
        prefix = "OWNED_D3D12_CONSERVATIVE_RASTER"
        paths = ("vs_dxil", "vs_dxbc", "vs_gs_dxil", "vs_gs_dxbc", "vs_hs_ds_dxil", "vs_hs_ds_dxbc",
                 "vs_hs_ds_gs_dxil", "vs_hs_ds_gs_dxbc", "ms_dxil", "as_ms_dxil")
        cases = ["ia_capture_parameter_integrity", "pso_validation", "spec_bring_up", "spec_half_pixel",
                 "spec_tir_forced4_coverage_0xf", "spec_msaa4_coverage_0xf_samplemask_0x5"]
        cases += [f"oracle_{path}" for path in paths] + ["cull_back", "depth_extrapolated_per_sample_clamped",
            "tier3_exact_grid_all_families", "tier3_large_coordinates", "tier3_degenerate_winding_cull_vertex0",
            "tier3_mesh_degenerate_winding_cull_vertex0", "tier3_depth_clip_degenerate_near_far",
            "tier3_inner_samplemask_forced4_dxbc_dxil", "tier3_off_geometry_on_inner_toggle", "tier3_helper_inner_zero",
            "tier3_early_late_depth_stencil_uav_queries"]
        lines = [prefix + "_BACKEND id=owned-graphics-d3d12", prefix + "_CAPS tier=3"]
        lines += [prefix + "_CASE_PASS " + case for case in cases]
        for path in paths:
            for samples in (1, 8):
                lines += [f"{prefix}_ORACLE path={path} samples={samples} triangles=64 covered=200 ambiguous=2 invocations=400",
                          f"{prefix}_DISTANCE path={path} samples={samples} covered=100 invocations=200"]
            lines += [f"{prefix}_DISTANCE_INTERPOLATION path={path} samples=8 indirect=1 mask=2 invocations=200"]
            lines += [f"{prefix}_VIEW path={path} mask={mask} samples=8 invocations=200" for mask in (0, 2, 3)]
            lines += [f"{prefix}_COMBINATION path={path} indirect={indirect} samples=8 ia_vertices=192" for indirect in (0, 1)]
            for case, samples in (("interior_direct", 1), ("outer_gap_indirect", 4), ("inner_boundary_indexed_mesh", 4),
                                  ("point_direct", 1), ("line_indirect_indexed_mesh", 4), ("corner_bundle", 1), ("interior_replay", 4)):
                count, mandatory = {"point_direct": (1, 1), "line_indirect_indexed_mesh": (21, 21), "corner_bundle": (2, 0)}.get(case, (100, 80))
                interior, edge, uncertainty = (0, count, 0) if count < 100 else (50, 49, 1)
                lines += [f"{prefix}_TIER3 path={path} case={case} samples={samples} covered={count} mandatory={mandatory} inner={interior} edge={edge} uncertain={uncertainty}"]
        lines += [prefix + "_DEPTH samples=4 checked=100 clamped=8 covered=25 max_error=1e-6",
                  prefix + "_TIER3_LARGE viewport=-16384,32768 covered=500 inner=450 edge=50"]
        for plane in ("near", "far"):
            for enabled in (1, 0):
                clip = "3e066666" if enabled else "3f31999a"
                depth = ("00000000" if plane == "near" else "3f800000") if enabled else ("39000000" if plane == "near" else "3f7ff800")
                lines += [f"{prefix}_DEPTH_CLIP path=vs_dxil plane={plane} enabled={enabled} covered={28 if enabled else 64} mandatory={10 if enabled else 64} "
                          f"probe_clip={clip} expected_clip={clip} probe_depth={depth} expected_depth={depth}"]
        for producer in ("dxil", "dxbc"):
            for case in ("early_depth_pass", "early_depth_fail", "late_depth_pass_output", "late_depth_fail_uav",
                         "early_stencil_pass", "early_stencil_fail", "late_stencil_pass", "late_stencil_fail_uav", "late_stencil_replace"):
                accepted = "fail" not in case
                suppressed = case.startswith("early_") and not accepted
                lines += [f"{prefix}_INTERACTION producer={producer} case={case} uav={0 if suppressed else 100} "
                          f"occlusion={400 if accepted else 0} accepted={int(accepted)} inner={0 if suppressed else 50}"]
        lines += [prefix + "_PASS paths=10 rejected=0 tier=3"]
        trace = "\n".join(lines) + "\n"
        self.assertEqual(runner.conservative_raster_failures(trace, ""), [])
        for line in lines[1:]:
            with self.subTest(line=line):
                self.assertTrue(runner.conservative_raster_failures(trace.replace(line + "\n", "", 1), ""))
                self.assertTrue(runner.conservative_raster_failures(trace + line + "\n", ""))
        for before, after in (("tier=3", "tier=1"), ("covered=100 mandatory=80", "covered=0 mandatory=80"),
                              ("inner=50 edge=49 uncertain=1", "inner=0 edge=99 uncertain=1"),
                              ("inner=0 edge=21 uncertain=0", "inner=1 edge=20 uncertain=0"),
                              ("occlusion=400 accepted=1", "occlusion=399 accepted=1"),
                              ("probe_clip=3e066666", "probe_clip=3f31999a"),
                              ("probe_depth=00000000", "probe_depth=39000000"),
                              ("uav=0 occlusion=0 accepted=0 inner=0", "uav=1 occlusion=0 accepted=0 inner=0")):
            self.assertTrue(runner.conservative_raster_failures(trace.replace(before, after), ""))
        self.assertTrue(runner.conservative_raster_failures(trace, prefix + "_FAIL step=depth"))


class DepthBoundsEvidence(unittest.TestCase):
    def test_terminal_only_cannot_claim_depth_bounds_gpu_proof(self):
        # The generic runner accepts a PASS substring. Neither a feature-cap line
        # nor an early terminal may substitute for the actual per-sample GPU oracles.
        trace = ("OWNED_D3D12_DEPTH_BOUNDS_BACKEND id=owned-graphics-d3d12\n"
                 "OWNED_D3D12_DEPTH_BOUNDS_CAP supported=1\n"
                 "OWNED_D3D12_DEPTH_BOUNDS_PASS cases=50\n")
        self.assertTrue(any("case matrix incomplete" in failure
                            for failure in runner.depth_bounds_failures(trace, "")))


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
        for event in self.events:
            if event["event"] == "wait":
                event.pop("submission", None)
        report = self.report()
        self.assertTrue(report["pass"], report["errors"])
        self.assertEqual(report["crossqueue"][0]["logical_wait"],
                         next(event["event_id"] for event in self.events if event["event"] == "wait"))
        self.assertTrue(report["crossqueue"][0]["consumer_encoded_before_producer_completion"])

    def test_history_reset_regression_rejected(self):
        next(e for e in self.events if e["event"] == "temporal_encode" and e["evaluation"] == 2)["reset"] = 1
        self.assertFalse(self.report()["pass"])

    def test_split_submission_retains_logical_execute_parent(self):
        for event in self.events:
            if event["event"] in ("execute_list", "encode") and event["queue"] == 2:
                event["submission"] = 19
        index = next(i for i, e in enumerate(self.events)
                     if e["event"] == "temporal_encode" and e["queue"] == 2)
        self.events[index:index] = [{"event": "submit", "queue": 2, "submission": 19},
                                   {"event": "complete", "queue": 2, "submission": 19, "status": 4}]
        for event_id, event in enumerate(self.events, 1):
            event["event_id"] = event_id
        report = self.report()
        self.assertTrue(report["pass"], report["errors"])
        self.assertEqual(report["crossqueue"][0]["submitted_recording"], 1)
        self.assertEqual(report["crossqueue"][0]["later_recordings_before_encode"], 96)

    def test_missing_logical_execute_parent_rejected(self):
        self.events = [e for e in self.events if not (e["event"] == "execute_list" and e["queue"] == 2)]
        self.assertFalse(self.report()["pass"])

    def test_newer_unrelated_execute_cannot_use_historical_parent(self):
        index = next(i for i, e in enumerate(self.events) if e["event"] == "encode" and e["queue"] == 2)
        self.events.insert(index, {"event": "execute_list", "queue": 2, "submission": 19,
                                   "list": 999, "recording": 1})
        for event_id, event in enumerate(self.events, 1):
            event["event_id"] = event_id
        self.assertFalse(self.report()["pass"])

    def test_encode_before_logical_execute_parent_rejected(self):
        encode = next(e for e in self.events if e["event"] == "encode" and e["queue"] == 2)
        execute = next(e for e in self.events if e["event"] == "execute_list" and e["queue"] == 2)
        encode["event_id"], execute["event_id"] = execute["event_id"], encode["event_id"]
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
