#!/usr/bin/env python3
"""Reject false-positive GPU reports using controlled failing traces; no GPU/Wine execution."""
from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import hashlib
import json
from pathlib import Path
import re
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
RAYTRACING_TRACE_CONTROL = "".join(
    f"OWNED_D3D12_RAYTRACING_TRACE_CONTROL path={path} deferred={deferred} rays=16 words=512 status=PASS\n"
    for path in ("native", "query") for deferred in (0, 1))
RAYTRACING_TRACE_CONTROL += "OWNED_D3D12_RAYTRACING_CASE trace-control status=PASS\n"
RAYTRACING_CONTRACT_TRACE += RAYTRACING_TRACE_CONTROL


class RunnerFailures(unittest.TestCase):
    def test_workgraph_inputs_freeze_the_selected_preview_sdk(self):
        for suite in ("work-graphs", "workgraphs-atomic64", "workgraphs-atomic64-typed"):
            with self.subTest(suite=suite), tempfile.TemporaryDirectory(dir=SCRATCH, prefix="workgraph-inputs-") as directory:
                base = Path(directory)
                runtime = base / "runtime"
                runtime.mkdir()
                (runtime / "owned-graphics-runtime.json").write_text('{"schema": 1}\n')
                fixture = base / "fixture.exe"
                fixture.write_bytes(controlled_pe_fixture())
                inputs = base / "selected-inputs"
                inputs.mkdir()
                for source in runner.suite_inputs(suite, fixture, inputs):
                    if source.is_relative_to(inputs) and source.name != "preview-d3d12.h":
                        source.write_bytes(b"controlled shader input")
                header = inputs / "preview-d3d12.h"
                original = b"controlled pinned preview SDK bytes\n"
                args = argparse.Namespace(runtime=runtime, lock=runtime / "owned-graphics-runtime.json",
                                          fixture=fixture, dxil_dir=inputs, out=base / "missing",
                                          suite=suite, mode="no-present", validation=True,
                                          api_validation=False, timeout=1)
                launches = []

                def fake_run(command, **kwargs):
                    if command[0] == str(runtime / "bin/wine"):
                        launches.append(command)
                        header.write_bytes(b"changed after input freeze")
                    return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

                with patch.object(runner.subprocess, "run", side_effect=fake_run):
                    with self.assertRaises(ValueError):
                        runner.run(args)
                    self.assertEqual(launches, [])
                    header.write_bytes(original)
                    args.out = base / "frozen"
                    with contextlib.redirect_stdout(io.StringIO()):
                        runner.run(args)
                self.assertEqual(len(launches), 1)
                self.assertEqual((args.out / "fixture/preview-d3d12.h").read_bytes(), original)
                self.assertNotEqual(header.read_bytes(), original)
                invocation = json.loads((args.out / "invocation.json").read_text())
                self.assertEqual(invocation["fixture_sha256"]["preview-d3d12.h"],
                                 hashlib.sha256(original).hexdigest())

    def test_multiqueue_waits_do_not_create_native_submissions(self):
        events = [{"event": kind} for kind, count in
                  (("execute_list", 64), ("signal_committed", 64), ("wait", 63), ("submit", 128), ("complete", 128))
                  for _ in range(count)]
        self.assertEqual(runner.multiqueue_submission_failures(events), [])
        self.assertTrue(runner.multiqueue_submission_failures(events + [{"event": "submit"}]))
        self.assertTrue(runner.multiqueue_submission_failures(events[:-1]))
        next(event for event in events if event["event"] == "wait")["submission"] = 99
        self.assertTrue(runner.multiqueue_submission_failures(events))
        folded = [{"event": kind, **({"folded": 1} if kind == "signal_committed" else {})} for kind, count in
                  (("execute_list", 64), ("signal_committed", 64), ("wait", 63), ("submit", 64), ("complete", 64))
                  for _ in range(count)]
        self.assertEqual(runner.multiqueue_submission_failures(folded), [])
        self.assertTrue(runner.multiqueue_submission_failures(folded + [{"event": "submit"}, {"event": "complete"}]))

    def test_automatic_pipeline_cache_scopes_same_executable_logs_and_producers(self):
        # Synthetic trace protocol only: NOT compilation, GPU or disk persistence evidence.
        prefix = "OWNED_D3D12_PIPELINE_CACHE"
        keys = {name: digit * 40 for name, digit in zip(("a", "b", "root", "concurrent", "control"), "12345")}
        keys["same_stage_node_one"] = "7" * 40
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
                  "disabled_second", "enabled", "clear_rebuild", "unaligned_input_lifetime", "returned_blob_isolation",
                  "distinct_compute_same_stage", "retained_after_stage_library_clear")
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
        logs = {("same_d3dalloy-d3d12.log" if phase == "parent" else phase + "/same_d3dalloy-d3d12.log"):
                "\n".join("OWNED_GRAPHICS_TRACE " + json.dumps(event) for event in events)
                for phase, events in phases.items()}
        self.assertEqual(runner.pipeline_cache_failures(stdout, "", logs), [])
        reordered_controls = [event for event in phases["pipeline-cache-control"]
                              if event["event"] != "pipeline_cache_auto_control"]
        reordered_controls += [{"event": "pipeline_cache_auto_control", "flags": flags, "hr": 0}
                               for flags in (4, 2, 1, 2)]
        reordered_logs = logs | {"pipeline-cache-control/same_d3dalloy-d3d12.log":
            "\n".join("OWNED_GRAPHICS_TRACE " + json.dumps(event) for event in reordered_controls)}
        self.assertEqual(runner.pipeline_cache_failures(stdout, "", reordered_logs), [])
        for label, corrupted in (
                ("second producer", logs | {"same_d3dalloy-d3d12.log": logs["same_d3dalloy-d3d12.log"] + "\nOWNED_GRAPHICS_TRACE " +
                    json.dumps({"event": "pipeline_cache_auto_begin", "key": keys["concurrent"]})}),
                ("missing disk log", {name: text for name, text in logs.items() if not name.startswith("pipeline-cache-disk/")}),
                ("native retry missing", logs | {"pipeline-cache-native/same_d3dalloy-d3d12.log":
                    logs["pipeline-cache-native/same_d3dalloy-d3d12.log"].replace("pipeline_cache_auto_retry", "unrelated_event")}),
                ("merged child log", {name.split("/")[-1]: text for name, text in logs.items()}),
                ("failed control", logs | {"pipeline-cache-control/same_d3dalloy-d3d12.log":
                    logs["pipeline-cache-control/same_d3dalloy-d3d12.log"] + "\nOWNED_GRAPHICS_TRACE " +
                    json.dumps({"event": "pipeline_cache_auto_control", "flags": 4, "hr": 0x80070005})}),
                ("disable not covered", logs | {"pipeline-cache-control/same_d3dalloy-d3d12.log":
                    logs["pipeline-cache-control/same_d3dalloy-d3d12.log"].replace('"flags": 1', '"flags": 2')}),
                ("disabled cache hit", logs | {"pipeline-cache-control/same_d3dalloy-d3d12.log":
                    logs["pipeline-cache-control/same_d3dalloy-d3d12.log"].replace("pipeline_cache_auto_bypass", "pipeline_cache_auto_hit")})):
            with self.subTest(label=label):
                self.assertTrue(runner.pipeline_cache_failures(stdout, "", corrupted))

    def test_pipeline_artifacts_require_real_operations_within_joined_phases(self):
        for suite, phases in (("pipeline-cache", [("distinct_compute", 1, 1, 0, 1, 1),
                              ("concurrent_distinct_compute", 1, 0, 0, 1, 1)]),
                              ("pipeline-library", [("concurrent_graphics", 2, 2, 0, 1, 2), ("root_abi", 2, 0, 0, 1, 2),
                               ("pixel_specialization", 1, 1, 1, 1, 0), ("constant_concurrent", 0, 0, 1, 1, 0),
                               ("constant_distinct", 0, 0, 1, 1, 0), ("constant_hit", 0, 0, 0, 0, 0),
                               ("constant_root_abi", 1, 0, 1, 1, 0), ("constant_entry", 1, 1, 1, 1, 0),
                               ("linked_geometry_cold", 2, 2, 0, 1, 2), ("linked_geometry_hit", 0, 0, 0, 0, 0),
                               ("linked_geometry_input", 2, 0, 0, 1, 1), ("captured_layout_cold", 6, 6, 0, 6, 11),
                               ("captured_layout_hit", 0, 0, 0, 0, 0), ("clear_rebuild", 2, 2, 0, 1, 2)])):
            prefix = runner.SUITE_PREFIXES[suite] + "_ARTIFACT_PHASE "
            rows = []
            native_kind = "native_compute" if suite == "pipeline-cache" else "native_render"
            for name, air, libraries, specializations, native, functions in phases:
                rows.append(prefix + "begin=" + name)
                for kind, count in (("shader_initialize", air), ("air_compile", air),
                                    ("new_library", libraries), ("specialize_function", specializations),
                                    ("new_function", functions), (native_kind, native)):
                    for index in range(count):
                        event={"event": "pipeline_artifact_operation", "kind": kind}
                        if name == "captured_layout_cold" and kind in ("air_compile","new_library"):
                            event.update(cache=1 if kind == "air_compile" else 2,key=f"{index+1+(100 if kind == 'new_library' else 0):040x}")
                        rows.append("info: OWNED_GRAPHICS_TRACE " + json.dumps(event))
                if name in ("captured_layout_cold","captured_layout_hit"):
                    for kind,base,cache in (("air_compile",0,1),("new_library",100,2)):
                        for index in (range(7) if name.endswith("hit") else (0,)):
                            rows.append("info: OWNED_GRAPHICS_TRACE "+json.dumps(dict(event="pipeline_artifact_hit",kind=kind,key=f"{base+index:040x}",cache=cache)))
                rows.append(prefix + "end=" + name)
            text = "\n".join(rows)
            self.assertEqual(runner.pipeline_artifact_failures(text, suite), [])
            # Cache IDs are runtime object identities, not fixed kind ordinals.
            relocated = text.replace('"cache": 1', '"cache": 41').replace('"cache": 2', '"cache": 73')
            self.assertEqual(runner.pipeline_artifact_failures(relocated, suite), [])
            for bad in ("", text.replace("pipeline_artifact_operation", "pipeline_artifact_hit"),
                        text + "\n" + rows[0], "\n".join(rows[1:]),
                        text.replace(prefix + "end=" + phases[0][0],
                                     "OWNED_GRAPHICS_TRACE " + json.dumps(
                                         {"event": "pipeline_artifact_operation", "kind": "air_compile"})
                                     + "\n" + prefix + "end=" + phases[0][0]),
                        text.replace(rows[0], prefix)):
                with self.subTest(suite=suite, bad=bad):
                    self.assertTrue(runner.pipeline_artifact_failures(bad, suite))
            for kind in ("new_function", native_kind):
                missing = "\n".join(line for line in rows if f'"kind": "{kind}"' not in line)
                with self.subTest(suite=suite, missing_operation=kind):
                    self.assertTrue(runner.pipeline_artifact_failures(missing, suite))
            if suite == "pipeline-library":
                for kind in ("air_compile","new_library"):
                    cold=next(line for line in rows if '"kind": "'+kind+'"' in line and '"key"' in line and '"event": "pipeline_artifact_operation"' in line)
                    warm=next(line for line in rows if '"kind": "'+kind+'"' in line and '"event": "pipeline_artifact_hit"' in line and '"key": "'+("0"*39+"1" if kind == "air_compile" else f"{101:040x}")+'"' in line)
                    sixth=cold.replace(f'{1 if kind == "air_compile" else 101:040x}',f'{6 if kind == "air_compile" else 106:040x}')
                    for changed in (text.replace(sixth,cold,1),text.replace(warm,warm.replace("pipeline_artifact_hit","ignored_work"),1),text.replace(warm,warm.replace(json.loads(warm.split("OWNED_GRAPHICS_TRACE ",1)[1])["key"],"f"*40),1),text.replace(warm,warm+"\n"+warm,1),text.replace(cold,cold.replace('"cache": '+("1" if kind == "air_compile" else "2"),'"cache": 3'),1)):
                        self.assertTrue(runner.pipeline_artifact_failures(changed,suite),kind)
                before,cold_and_after=text.split(prefix+"begin=captured_layout_cold",1)
                self.assertTrue(runner.pipeline_artifact_failures(before+prefix+"begin=captured_layout_cold"+cold_and_after.replace("pipeline_artifact_operation","ignored_work",1),suite))
                extra="OWNED_GRAPHICS_TRACE "+json.dumps(dict(event="pipeline_artifact_operation",kind="shader_initialize"))
                self.assertTrue(runner.pipeline_artifact_failures(text.replace(prefix+"end=captured_layout_hit",extra+"\n"+prefix+"end=captured_layout_hit",1),suite))
                for operation in ("shader_initialize", "air_compile", "new_library", "specialize_function",
                                  "new_function", "native_compute", "native_render"):
                    unexpected = "OWNED_GRAPHICS_TRACE " + json.dumps(
                        {"event": "pipeline_artifact_operation", "kind": operation})
                    with self.subTest(unexpected_warm_operation=operation):
                        self.assertTrue(runner.pipeline_artifact_failures(
                            text.replace(prefix + "end=constant_hit",
                                         unexpected + "\n" + prefix + "end=constant_hit"), suite))
                self.assertTrue(runner.pipeline_artifact_failures(
                    text.replace("specialize_function", "new_library"), suite))

    def test_render_pass_final_discard_requires_native_stores_and_preserved_pixels(self):
        # Controlled native/queue protocol input, not observed GPU evidence.
        prefix = "OWNED_D3D12_RENDER_PASSES_FINAL_DISCARD"
        output, trace = [], []
        serial = 0
        for suspended in range(2):
            for replay in range(2):
                submission = 100 + 2 * suspended + replay
                output.append(f"{prefix} suspended={suspended} replay={replay} color=ff003000 "
                              "pixels=16 depth_test=1 stencil_test=1")
                trace.append(f"{prefix}_PHASE begin suspended={suspended} replay={replay}")
                count = 3 if suspended else 2
                for index in range(count):
                    serial += 1
                    stores = (0, 1, 0, 0) if index == count - 1 else (1, 1, 1, 1)
                    for (aspect, slot, texture), store in zip(
                            ((4, 0, 700), (4, 1, 701), (1, 0, 702), (2, 0, 702)), stores):
                        trace.append(f"OWNED_GRAPHICS_NATIVE_RENDER_ATTACHMENT encoder={900 + serial} "
                                     f"aspect={aspect} slot={slot} texture={texture} load=1 store={store}")
                    trace.append("OWNED_GRAPHICS_TRACE " + json.dumps(
                        dict(event="native_render_begin", queue=9, submission=submission, encoder=serial,
                             members=1, begin_count=1)))
                    trace.append("OWNED_GRAPHICS_TRACE " + json.dumps(
                        dict(event="native_render_end", submission=submission, encoder=serial, end_count=1)))
                trace.append("OWNED_GRAPHICS_TRACE " + json.dumps(
                    dict(event="complete", queue=9, submission=submission, status=4)))
                trace.append(f"{prefix}_PHASE end suspended={suspended} replay={replay}")
        stdout, stderr = "\n".join(output), "\n".join(trace)
        self.assertEqual(runner.render_pass_discard_failures(stdout, stderr), [])
        for index, row in enumerate(trace):
            with self.subTest(missing_row=index):
                self.assertTrue(runner.render_pass_discard_failures(
                    stdout, "\n".join(trace[:index] + trace[index + 1:])))
            if row.startswith("OWNED_GRAPHICS_NATIVE_RENDER_ATTACHMENT "):
                store = int(row.rsplit("=", 1)[1])
                mutations = [
                    row.rsplit("=", 1)[0] + f"={1 - store}",
                    row.replace("load=1", "load=0"),
                    row.replace("encoder=", "ignored="),
                    row.replace("texture=70", "texture=80"),
                    row + "\n" + row,
                    "OWNED_GRAPHICS_TRACE " + json.dumps(
                        dict(event="native_render_attachment", slot=0, load=1, store=store))]
                for changed in mutations:
                    with self.subTest(native_row=index, changed=changed):
                        self.assertTrue(runner.render_pass_discard_failures(
                            stdout, "\n".join(trace[:index] + [changed] + trace[index + 1:])))
        for row in output:
            with self.subTest(pixel_row=row):
                self.assertTrue(runner.render_pass_discard_failures(stdout.replace(row, ""), stderr))
                self.assertTrue(runner.render_pass_discard_failures(
                    stdout.replace(row, row.replace("color=ff003000", "color=ff001000")), stderr))
        self.assertTrue(runner.render_pass_discard_failures(stdout, stderr.replace('"status": 4', '"status": 5')))
        recording_row = next(row for row in trace if '"native_render_begin"' in row)
        for malformed in ("[]", "{", '{"event":"native_render_begin","queue":[]}'):
            with self.subTest(malformed=malformed):
                self.assertTrue(runner.render_pass_discard_failures(
                    stdout, stderr.replace(recording_row, "OWNED_GRAPHICS_TRACE " + malformed, 1)))

    def test_execution_graph_requires_gpu_values_and_retired_native_operations(self):
        # Independent log input from the frozen ExecutionGraph fixture/oracle, not
        # runner-owned expectations. The generator emits records, never verifies them.
        identity = list(range(8))
        render = [
            ("graph-boundary-0", 2, [2], [identity, identity], 0, 0, 0, 0, [4], 0, False),
            ("graph-boundary-1", 2, [1, 1], [], 2, 0, 0, 0, [4], 0, False),
            ("graph-boundary-2", 2, [1, 1], [], 2, 0, 0, 0, [4], 0, False),
            ("graph-boundary-3", 2, [1, 1], [], 2, 1, 0, 0, [4], 0, False),
            ("graph-boundary-4", 2, [1, 1], [], 2, 0, 0, 0, [4, 4], 0, False),
            ("graph-uav-dependency", 1, [1, 1], [], 2, 0, 0, 0, [4], 0, False),
            ("graph-uav-war", 1, [1, 1], [], 2, 0, 0, 0, [4], 0, False),
            ("graph-uav-waw", 1, [1, 1], [], 2, 0, 0, 0, [4], 0, False),
            ("graph-uav-atomic", 1, [1, 1], [], 2, 0, 0, 0, [4], 0, False),
            ("graph-uav-vertex", 1, [2], [identity, identity], 0, 0, 0, 0, [4], 0, True),
            ("graph-access-0", 2, [2], [identity, identity], 0, 1, 0, 0, [], 0, False),
            ("graph-access-1", 2, [2], [identity, identity], 0, 1, 0, 0, [], 0, False),
            ("graph-access-2", 2, [2], [identity, identity], 0, 0, 1, 0, [], 0, False),
            ("graph-access-3", 2, [2], [identity, identity], 0, 1, 0, 0, [], 0, False),
            ("graph-frontier-256", 2, [], [], 0, 256, 0, 0, [], 0, False),
            ("graph-frontier-512", 2, [], [], 0, 512, 0, 0, [], 0, False),
            ("graph-frontier-boundary-0", 2, [2, 2], [identity, identity, identity, identity], 0, 61, 0, 0, [4], 0, False),
            ("graph-frontier-boundary-1", 2, [2], [identity, identity], 0, 63, 0, 1, [4], 0, False),
            ("graph-mips", 1, [2], [identity, identity], 0, 0, 1, 0, [], 0, False),
            ("graph-union-0", 2, [3], [identity, [1, 0, 2, 3, 4, 5, 6, 7], identity], 0, 0, 0, 0, [], 0, False),
            ("graph-union-1", 2, [3], [[0, 2, 2, 3, 4, 5, 6, 7], [2, 1, 2, 3, 4, 5, 6, 7], identity], 0, 0, 0, 0, [], 0, False),
            ("graph-union-lifetime", 1, [3], [[0, 2, 2, 3, 4, 5, 6, 7], [2, 1, 2, 3, 4, 5, 6, 7], identity], 0, 0, 0, 0, [], 0, False),
            ("graph-clears-0", 2, [], [], 0, 0, 0, 1, [4, 4, 1, 2], 3, False),
            ("graph-clears-1", 2, [], [], 0, 1, 0, 2, [4, 4, 4, 1, 2], 2, False),
            ("graph-clear-view-0", 2, [1], [], 1, 0, 0, 1, [4], 1, False),
            ("graph-clear-view-1", 2, [1], [], 1, 0, 0, 1, [4], 1, False)]
        enhanced = [(f"graph-enhanced-scope-{c}", 1, [2] if c < 2 else [1, 1],
                     [identity, identity] if c < 2 else [], 0 if c < 2 else 2,
                     0, 0, 0, [], 0, False) for c in range(5)]
        safety = [("graph-clear-reset-release", 1, [], [], 0, 2, 0, 2, [4, 4], 2, False)]
        render_numeric = [f"CROSS_LIST control={c} replay={r} left={('ff102000' if c == 4 else 'ff402080')} "
                          f"right={('ff100000' if c == 4 else 'ff400080')} exact=1 sealed=1"
                          for c in range(5) for r in range(2)]
        for c in range(5):
            render_numeric += [f"CROSS_LIST_UAV rgba={('ff402080' if c == 0 else 'ff400080')} exact=1 dependency=1",
                               f"GRAPH_UAV control={c} word={6 if c == 4 else 64 if c == 2 else 32} render_encoders={1 if c == 4 else 2} "
                               f"members=2 inline_barriers={int(c == 4)} exact=1"]
        render_numeric += [f"GRAPH_ACCESS control={c} replay={r} rgba=ff000050 unrelated={('13579bdf' if c == 2 else '00000010')} "
                           f"render_encoders=1 members=2 exact=1" for c in range(4) for r in range(2)]
        render_numeric += [f"GRAPH_FRONTIER count={count} replay={replay} word=13579bdf exact=1 sealed=1"
                           for count in (256, 512) for replay in range(2)]
        render_numeric += [f"GRAPH_FRONTIER_BOUNDARY delayed_clear={delayed} replay={replay} "
                           f"rgba={'ff000020' if delayed else 'ff000040'} render_encoders={1 if delayed else 2} "
                           f"clear_encoders={delayed} exact=1 sealed=1" for delayed in range(2) for replay in range(2)]
        render_numeric += ["GRAPH_MIPS rgba=ff000050 mip0=16 mip1=64 mip1_untouched=16 render_encoders=1 members=2 exact=1"]
        render_numeric += [f"GRAPH_UNION unused={u} replay={r} first={('ff000030' if u else 'ff004030')} "
                           f"second={('ff005000' if u else 'ff003040')} render_encoders=1 members=3 identity_reset=1 exact=1"
                           for u in range(2) for r in range(2)]
        render_numeric += ["GRAPH_UNION_LIFETIME first=ff000030 second=ff005000 render_encoders=1 members=3 "
                           "multi_pso=1 owner_released=1 reset_before_gate=1 exact=1"]
        render_numeric += [f"GRAPH_CLEARS observation={o} replay={r} color0=ff200000 color1=ff004000 depth=3f000000 "
                           f"stencil=22 clear_encoders={1+o} clear_loads={4+o} superseded={3-o} no_draw_final_flush=1 exact=1"
                           for o in range(2) for r in range(2)]
        render_numeric += [f"GRAPH_CLEAR_VIEW order={order} replay={replay} untouched=ffff0000 drawn=ff00ff00 "
                           "expected_untouched=ffff0000 expected_drawn=ff00ff00"
                           for order in range(2) for replay in range(2)]
        enhanced_numeric = [f"GRAPH_SCOPES control={c} left=ff00ff00 right=ff0000ff expected_render_encoders={1 if c < 2 else 2} exact=1"
                            for c in range(5)]
        safety_numeric = ["GRAPH_CLEAR_RESET_RELEASE_PASS old=ff200000 new=ff004000 recordings=2 clear_encoders=2 superseded=2 released_before_retirement=1"]
        for suite, base, phases, numeric in (
                ("render-passes", "OWNED_D3D12_RENDER_PASSES", render, render_numeric),
                ("enhanced-barriers", "OWNED_D3D12_ENHANCED_BARRIERS", enhanced, enhanced_numeric),
                ("execution-safety", "OWNED_D3D12_EXECUTION_SAFETY", safety, safety_numeric)):
            chunks = []
            for name, replays, begins, mappings, identities, blits, computes, clears, aspects, superseded, inline in phases:
                for replay in range(replays):
                    events = [{"event": "submission_clear_superseded", "aspect": 4} for _ in range(superseded)]
                    if name.startswith("graph-frontier-"):
                        events.append({"event": "submission_plan_work", "history_checks": 0,
                                       "candidate_checks": 0, "metadata_copies": 0, "scope_checks": 0, "feedback_checks": 0})
                        pairs = [(62, 63), (64, 65)] if name == "graph-frontier-boundary-0" else [(64, 65)] if name == "graph-frontier-boundary-1" else []
                        events += [{"event": "submission_render_merged", "before_list": before, "after_list": after, "members": 2} for before, after in pairs]
                        if name == "graph-frontier-boundary-0":
                            events.append({"event": "submission_clear_load", "clear_node": 0, "render_node": 62, "aspect": 4})
                    for kind, count in (("render", len(begins)), ("clear", clears), ("blit", blits), ("compute", computes)):
                        for index in range(count):
                            events.append({"event": f"native_{kind}_begin", "begin_count": 1,
                                           **({"members": begins[index]} if kind == "render" else {})})
                            if kind == "render":
                                events += [{"event": "native_render_attachment", "slot": slot, "load": 1, "store": 1}
                                           for slot in range(2 if name.startswith("graph-union-") else 1)]
                            events.append({"event": f"native_{kind}_wait", "wait_count": 1,
                                           **({"stages": 3 if inline else 2} if kind == "render"
                                              else {"stages": 2} if kind == "clear" else {})})
                            if index == 0 and kind == ("clear" if name == "graph-frontier-boundary-1" else "render" if begins else "clear"):
                                events += [{"event": "native_clear_load", "clear_count": 1, "aspect": a,
                                            **({"node": 63} if name == "graph-frontier-boundary-1" else {})} for a in aspects]
                            if kind == "render":
                                if identities:
                                    events.append({"event": "native_render_color_map_identity", "map_count": 1})
                                for member in range(begins[index]):
                                    if mappings:
                                        if inline and member == 1:
                                            events.append({"event": "native_render_inline_barrier", "member": 1,
                                                           "scope": 1, "before": 1, "after": 1, "barrier_count": 1})
                                        events.append({"event": "native_render_color_map", "map_count": 1, "member": member,
                                                       **{f"physical{i}": value for i, value in enumerate(mappings[member])}})
                                    if member:
                                        events.append({"event": "native_render_member"})
                            if kind in ("blit", "compute"):
                                events.append({"event": f"native_{kind}_encode", "encode_count": 1})
                            events.append({"event": f"native_{kind}_update", "update_count": 1,
                                           **({"stages": 3 if inline and kind == "render" else 2} if kind in ("render", "clear") else {})})
                            events.append({"event": f"native_{kind}_end", "end_count": 1})
                    chunks.append([base + f"_GRAPH_PHASE begin={name} replay={replay}",
                                   *["OWNED_GRAPHICS_TRACE " + json.dumps(e) for e in events],
                                   base + f"_GRAPH_PHASE end={name} replay={replay}"])
            rows = [line for chunk in chunks for line in chunk]
            numeric = [base + "_" + line for line in numeric]
            stdout, stderr = "\n".join(numeric), "\n".join(rows)
            with self.subTest(suite=suite, valid=True):
                self.assertEqual(runner.execution_graph_failures(stdout, stderr, suite), [])
            mutations = [("summary-only", stdout, ""), ("no-readback", "", stderr),
                         ("malformed-phase", stdout, stderr.replace(" replay=0", " replay=invalid", 1)),
                         ("malformed-json", stdout, stderr.replace("OWNED_GRAPHICS_TRACE {", "OWNED_GRAPHICS_TRACE [", 1)),
                         ("non-object", stdout, stderr.replace(rows[1], "OWNED_GRAPHICS_TRACE []", 1))]
            for i, chunk in enumerate(chunks):
                mutations += [(f"missing-phase-{i}", stdout, "\n".join(line for j, c in enumerate(chunks) if j != i for line in c)),
                              (f"duplicate-phase-{i}", stdout, stderr + "\n" + "\n".join(chunk)),
                              (f"missing-begin-{i}", stdout, stderr.replace(chunk[0] + "\n", "", 1)),
                              (f"missing-end-{i}", stdout, stderr.replace(chunk[-1], "", 1))]
                if i:
                    reordered = chunks.copy()
                    reordered[i-1], reordered[i] = reordered[i], reordered[i-1]
                    mutations.append((f"reordered-phase-{i}", stdout, "\n".join(line for c in reordered for line in c)))
            seen = set()
            for row in rows:
                if not row.startswith("OWNED_GRAPHICS_TRACE "):
                    continue
                event = json.loads(row.split("OWNED_GRAPHICS_TRACE ", 1)[1])
                kind = event["event"]
                if kind == "native_render_attachment":
                    continue
                if kind in seen:
                    continue
                seen.add(kind)
                mutations += [("missing-" + kind, stdout, stderr.replace(row + "\n", "", 1)),
                              ("duplicate-" + kind, stdout, stderr.replace(row, row + "\n" + row, 1)),
                              ("metadata-not-" + kind, stdout, stderr.replace(row,
                               'OWNED_GRAPHICS_TRACE {"event":"native_render_attachment","slot":0,"load":1,"store":1}', 1))]
                for key in event:
                    if key.endswith("_count") or key in ("stages", "scope", "before", "after", "member", "members", "aspect"):
                        for value in (None, 255, True):
                            bad = {**event, key: value}
                            mutations.append((f"{kind}-{key}-{value}", stdout,
                                              stderr.replace(row, "OWNED_GRAPHICS_TRACE " + json.dumps(bad), 1)))
                if kind == "native_render_color_map":
                    for value in (-1, 8, 255, True, None):
                        bad = {**event, "physical7": value}
                        mutations.append((f"bad-native-index-{value}", stdout,
                                          stderr.replace(row, "OWNED_GRAPHICS_TRACE " + json.dumps(bad), 1)))
                    mutations.append(("map-is-not-identity-event", stdout,
                                      stderr.replace('"native_render_color_map"', '"native_render_color_map_identity"', 1)))
                if kind == "native_render_color_map_identity":
                    mutations.append(("identity-is-not-mapped-member", stdout,
                                      stderr.replace('"native_render_color_map_identity"', '"native_render_color_map"', 1)))
            for i, line in enumerate(numeric):
                mutations += [(f"missing-readback-{i}", stdout.replace(line, "", 1), stderr),
                              (f"duplicate-readback-{i}", stdout + "\n" + line, stderr),
                              (f"wrong-readback-{i}", stdout.replace(line, line.replace("=ff", "=fe", 1)
                               if "=ff" in line else line.replace("word=", "word=9", 1), 1), stderr)]
            if suite == "render-passes":
                for chunk in (c for c in chunks if "begin=graph-frontier-" in c[0]):
                    frontier_chunk = "\n".join(chunk)
                    for row in chunk:
                        if not row.startswith("OWNED_GRAPHICS_TRACE "):
                            continue
                        event = json.loads(row.split("OWNED_GRAPHICS_TRACE ", 1)[1])
                        kind = event["event"]
                        if kind in ("submission_plan_work", "submission_render_merged", "submission_clear_load"):
                            mutations += [("frontier-missing-" + kind, stdout, stderr.replace(frontier_chunk, frontier_chunk.replace(row + "\n", "", 1))),
                                          ("frontier-duplicate-" + kind, stdout, stderr.replace(frontier_chunk, frontier_chunk.replace(row, row + "\n" + row, 1)))]
                        if kind == "submission_plan_work":
                            for field in ("history_checks", "candidate_checks", "metadata_copies", "scope_checks", "feedback_checks"):
                                for value in (None, True, -1):
                                    bad = "OWNED_GRAPHICS_TRACE " + json.dumps({**event, field: value})
                                    mutations.append((f"frontier-invalid-{field}-{value}", stdout, stderr.replace(frontier_chunk, frontier_chunk.replace(row, bad, 1))))
                            if "boundary" not in chunk[0]:
                                count = 256 if "frontier-256" in chunk[0] else 512
                                for field, value in (("history_checks", count * (count - 1) // 2), ("history_checks", event["history_checks"] + 1),
                                                     ("candidate_checks", 1), ("metadata_copies", 1), ("scope_checks", 1), ("feedback_checks", 1)):
                                    bad = "OWNED_GRAPHICS_TRACE " + json.dumps({**event, field: value})
                                    mutations.append((f"frontier-work-{count}-{field}-{value}", stdout, stderr.replace(frontier_chunk, frontier_chunk.replace(row, bad, 1))))
                        elif kind == "submission_render_merged":
                            for field, value in (("before_list", 63), ("after_list", 64), ("members", 4), ("members", True)):
                                bad = "OWNED_GRAPHICS_TRACE " + json.dumps({**event, field: value})
                                mutations.append((f"frontier-cross-partition-{field}-{value}", stdout, stderr.replace(frontier_chunk, frontier_chunk.replace(row, bad, 1))))
                        elif kind == "submission_clear_load":
                            for field, value in (("clear_node", 63), ("render_node", 64), ("aspect", 1)):
                                bad = "OWNED_GRAPHICS_TRACE " + json.dumps({**event, field: value})
                                mutations.append(("frontier-clear-position-" + field, stdout, stderr.replace(frontier_chunk, frontier_chunk.replace(row, bad, 1))))
                        elif kind == "native_clear_load" and "boundary-1" in chunk[0]:
                            bad = "OWNED_GRAPHICS_TRACE " + json.dumps({**event, "node": 64})
                            promoted = 'OWNED_GRAPHICS_TRACE {"event":"submission_clear_load","clear_node":63,"render_node":64,"aspect":4}'
                            mutations += [("frontier-delayed-clear-wrong-node", stdout, stderr.replace(frontier_chunk, frontier_chunk.replace(row, bad, 1))),
                                          ("frontier-delayed-clear-promoted", stdout, stderr.replace(frontier_chunk, frontier_chunk.replace(row, promoted, 1))),
                                          ("frontier-delayed-clear-extra-promotion", stdout, stderr.replace(frontier_chunk, frontier_chunk.replace(row, row + "\n" + promoted, 1)))]
                        elif kind == "native_render_color_map" and "boundary-0" in chunk[0] and event["member"] == 0:
                            bad = "OWNED_GRAPHICS_TRACE " + json.dumps({**event, "member": 2})
                            mutations.append(("frontier-group-map-no-reset", stdout, stderr.replace(frontier_chunk, frontier_chunk.replace(row, bad, 1))))
                mutations += [("clear-view-old-red", stdout.replace("untouched=ffff0000", "untouched=ff0000ff"), stderr),
                              ("clear-view-draw-corrupt", stdout.replace("drawn=ff00ff00", "drawn=ff0000ff"), stderr)]
                for chunk in (c for c in chunks if "begin=graph-clear-view-" in c[0]):
                    view_chunk = "\n".join(chunk)
                    for row in chunk:
                        if not row.startswith("OWNED_GRAPHICS_TRACE ") or '"native_render_attachment"' in row:
                            continue
                        event = json.loads(row.split("OWNED_GRAPHICS_TRACE ", 1)[1])
                        mutations += [("clear-view-missing-" + event["event"], stdout,
                                       stderr.replace(view_chunk, view_chunk.replace(row + "\n", "", 1))),
                                      ("clear-view-extra-" + event["event"], stdout,
                                       stderr.replace(view_chunk, view_chunk.replace(row, row + "\n" + row, 1)))]
                lifetime_chunk = "\n".join(next(c for c in chunks if "begin=graph-union-lifetime " in c[0]))
                for field in ("multi_pso", "owner_released", "reset_before_gate"):
                    mutations += [("lifetime-disabled-" + field, stdout.replace(field + "=1", field + "=0"), stderr),
                                  ("lifetime-missing-" + field, stdout.replace(field + "=1 ", ""), stderr)]
                for row in lifetime_chunk.splitlines():
                    if not row.startswith("OWNED_GRAPHICS_TRACE ") or '"native_render_attachment"' in row:
                        continue
                    event = json.loads(row.split("OWNED_GRAPHICS_TRACE ", 1)[1])
                    mutations += [("lifetime-missing-" + event["event"], stdout,
                                   stderr.replace(lifetime_chunk, lifetime_chunk.replace(row + "\n", "", 1))),
                                  ("lifetime-duplicate-" + event["event"], stdout,
                                   stderr.replace(lifetime_chunk, lifetime_chunk.replace(row, row + "\n" + row, 1)))]
                atomic_chunk = "\n".join(next(c for c in chunks if "begin=graph-uav-atomic " in c[0]))
                vertex_chunk = "\n".join(next(c for c in chunks if "begin=graph-uav-vertex " in c[0]))
                barrier_row = next(row for row in vertex_chunk.splitlines() if '"native_render_inline_barrier"' in row)
                mutations += [("wrong-final-map", stdout, stderr.replace('"physical7": 7', '"physical7": 6')),
                              ("fragment-atomic-collapsed", stdout, stderr.replace(atomic_chunk,
                               vertex_chunk.replace("graph-uav-vertex", "graph-uav-atomic").replace('"stages": 3', '"stages": 2'))),
                              ("fragment-atomic-inline", stdout, stderr.replace(atomic_chunk, atomic_chunk.replace(
                               base + "_GRAPH_PHASE end=", barrier_row + "\n" + base + "_GRAPH_PHASE end=", 1))),
                              ("old-fragment-inline-scope", stdout, stderr.replace(barrier_row,
                               barrier_row.replace('"scope": 1', '"scope": 3').replace('"before": 1', '"before": 2').replace('"after": 1', '"after": 2'))),
                              ("vertex-fragment-fence", stdout, stderr.replace(vertex_chunk, vertex_chunk.replace('"stages": 3', '"stages": 2'))),
                              ("vertex-fragment-producer", stdout, stderr.replace(barrier_row, barrier_row.replace('"before": 1', '"before": 2'))),
                              ("vertex-fragment-consumer", stdout, stderr.replace(barrier_row, barrier_row.replace('"after": 1', '"after": 2'))),
                              ("old-atomic-numeric", stdout.replace("control=3 word=32 render_encoders=2 members=2 inline_barriers=0",
                               "control=3 word=32 render_encoders=1 members=2 inline_barriers=1"), stderr),
                              ("wrong-vertex-word", stdout.replace("control=4 word=6", "control=4 word=32"), stderr),
                              ("wrong-UAV-word", stdout.replace("word=32", "word=31"), stderr),
                              ("wrong-mip", stdout.replace("mip1=64", "mip1=16"), stderr),
                              ("wrong-depth", stdout.replace("depth=3f000000", "depth=3f800000"), stderr),
                              ("old-root-srv-opaque", stdout.replace("control=3 replay=0 rgba=ff000050 unrelated=00000010 render_encoders=1",
                               "control=3 replay=0 rgba=ff000050 unrelated=00000010 render_encoders=2"), stderr),
                              ("old-15-phase-matrix", stdout, "\n".join(line for c in chunks[14:20] + chunks[22:] for line in c))]
            elif suite == "execution-safety":
                eager = [row for row in rows if '"submission_clear_superseded"' not in row]
                eager[1:1] = [row for row in rows if '"native_clear_' in row and '"native_clear_load"' not in row]
                mutations += [("old-reset-output", stdout.replace("old=ff200000", "old=ff004000"), stderr),
                              ("eager-clear-four-no-superseded", stdout.replace("clear_encoders=2 superseded=2",
                               "clear_encoders=4 superseded=0"), "\n".join(eager)),
                              ("new-reset-output", stdout.replace("new=ff004000", "new=ff200000"), stderr),
                              ("not-released", stdout.replace("released_before_retirement=1", "released_before_retirement=0"), stderr)]
            for label, out, err in mutations:
                with self.subTest(suite=suite, label=label):
                    self.assertTrue(runner.execution_graph_failures(out, err, suite))

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
OWNED_D3D12_FSR_FG_DEBUG_REPLAY_PASS combined=1 debug_overlay=full_display replay=fresh_native context_destroy_before_replay=1
OWNED_D3D12_FSR_FG_COUNT_REJECTION_PASS invalid=0,5 unsupported=2,3,4 sentinel=exact prepare=retained
OWNED_D3D12_FSR_FG_NO_NOTIFY_PROXY_PASS frame=3 generated=1
OWNED_D3D12_FSR_FG_PREPARE_ORDER_PASS matching_frame=2 newest_prepare=3 output=exact
OWNED_D3D12_FSR_FG_NATIVE_PASS count=1 distinct_gpu_outputs=1 rect_guard=exact reset_output=current
OWNED_D3D12_FSR_FG_NATIVE_PACING_PASS count=1 generated_before_real=1 bootstrap_reset=native_output disabled=real_only callback_error=real_only
OWNED_D3D12_FSR_FG_ORIGINAL_SDK_PASS resize1=unsupported unchanged=1 resize=implemented pacing=original
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
                (9, 1, 0, 1, 0), (10, 1, 0, 0, 0), (11, 1, 0, 0, 0), (12, 1, 1, 0, 0)):
            trace += (f"OWNED_D3D12_FSR_FG_PRESENT_CALL frame={frame} enabled={enabled} only_generated={only} "
                      f"generation_error={gen_error} present_error={present_error} "
                      f"qpc_start={frame * 100} qpc_end={frame * 100 + 10} frequency=1000\n")
        for frame, generated, index in ((1, 1, 1), (1, 0, 0), (2, 1, 1), (2, 0, 0),
                (3, 1, 1), (3, 0, 0), (4, 1, 1), (4, 0, 0), (5, 0, 0), (6, 0, 0),
                (7, 1, 1), (7, 0, 0), (8, 1, 1), (9, 0, 0), (10, 1, 1), (10, 0, 0),
                (11, 1, 1), (11, 0, 0), (12, 1, 1)):
            trace += (f"OWNED_D3D12_FSR_FG_CALLBACK_PIXELS frame={frame} generated={generated} "
                      f"output_index={index} width=128 height=128 hash={frame * 16 + index:016x} marker={frame + 32},{190 if generated else 70},220\n")
        for stream, frames in (("callback", list(range(1, 13))), ("builtin_ui", list(range(11, 15)))):
            for frame in frames:
                submitted = (sum(1 + int(f not in (5, 6, 9)) - int(f in (8, 12))
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
        self.assertTrue(self.simulate_metalfx("fsr-fg", trace)["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("result=0x887a000b", "result=0x00000000", 1))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("qpc_end=20000", "qpc_end=1"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("output_vblanks=3", "output_vblanks=0"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("resize1=unsupported", "resize1=success"))["pass"])
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
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("frame=3 generated=1 output_index=1", "frame=3 generated=1 output_index=2"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("frame=8 generated=1 output_index=1", "frame=8 generated=0 output_index=0"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("hash=0000000000000031", "hash=invalid"))["pass"])
        self.assertFalse(self.simulate_metalfx("fsr-fg", trace.replace("reset_output=current", "reset_output=unchanged"))["pass"])

    def test_fsr_real_three_channel_history_and_centered_cap_evidence(self):
        # Controlled parser input based on runtime02; not GPU execution evidence.
        trace = """OWNED_D3D12_TEMPORAL_BACKEND id=owned-graphics-d3d12
OWNED_D3D12_FSR_FRAME_PASS frame=0 variant=direct input_tag=96 sentinel=1
OWNED_D3D12_FSR_FRAME_PASS frame=1 variant=direct input_tag=160 sentinel=1
OWNED_D3D12_FSR_FRAME_PASS frame=2 variant=direct input_tag=96 sentinel=1
OWNED_D3D12_FSR_FRAME_PASS frame=3 variant=direct input_tag=96 sentinel=1
OWNED_D3D12_FSR_FRAME_PASS frame=0 variant=device_proxy input_tag=96 sentinel=1
OWNED_D3D12_FSR_FRAME_PASS frame=1 variant=device_proxy input_tag=160 sentinel=1
OWNED_D3D12_FSR_FRAME_PASS frame=2 variant=device_proxy input_tag=96 sentinel=1
OWNED_D3D12_FSR_FRAME_PASS frame=3 variant=device_proxy input_tag=96 sentinel=1
OWNED_D3D12_FSR_CASE_PASS case=device_proxy_underlying_gpu result=0
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
        proxy_frame = "OWNED_D3D12_FSR_FRAME_PASS frame=2 variant=device_proxy input_tag=96 sentinel=1\n"
        proxy_complete = "OWNED_D3D12_FSR_CASE_PASS case=device_proxy_underlying_gpu result=0\n"
        for bad in (trace.replace(proxy_frame, ""), trace.replace(proxy_frame, proxy_frame * 2),
                    trace.replace(proxy_frame, proxy_frame.replace("device_proxy", "direct")),
                    trace.replace(proxy_complete, ""), trace.replace(proxy_complete, proxy_complete * 2)):
            with self.subTest(proxy_contract=bad):
                self.assertFalse(self.simulate_metalfx("fsr", bad)["pass"])

    def test_fsr_summary_without_full_sr_and_centered_cap_evidence_cannot_pass(self):
        trace = "OWNED_D3D12_TEMPORAL_BACKEND id=owned-graphics-d3d12\nOWNED_D3D12_FSR_PASS\n"
        for variant in ("direct", "device_proxy"):
            for frame in range(4):
                trace += f"OWNED_D3D12_FSR_FRAME_PASS frame={frame} variant={variant}\n"
        for replay, changed in ((0, 0), (1, 0), (2, 1)):
            trace += f"OWNED_D3D12_FSR_REPLAY_PASS replay={replay} pixels=exact destroyed_before_submit=1 sentinel=1 changed_gpu_input={changed}\n"
        for format in (0, 54, 41):
            trace += f"OWNED_D3D12_FSR_EXPOSURE_PASS format={format} value=1 pixels=exact sentinel=1\n"
        trace += "OWNED_D3D12_FSR_CASE_PASS case=rejected_dispatch_output_unchanged result=0\n"
        trace += "OWNED_D3D12_FSR_CASE_PASS case=device_proxy_underlying_gpu result=0\n"
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
                    (args.out / "owned-d3d12-ngx_d3dalloy-d3d12.log").write_text(backend_log)
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
        self.assertTrue(self.simulate_raytracing()["pass"])
        markers = ["OWNED_D3D12_RAYTRACING_CASE hitobject status=PASS\n"]
        markers += RAYTRACING_TRACE_CONTROL.splitlines(keepends=True)
        for marker in markers:
            for replacement in ("", marker + marker, marker.replace("PASS", "FAIL")):
                with self.subTest(marker=marker, replacement=replacement):
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
                 ("as_child3d", (0, 0, 0, 0, 8, 32), 8),
                 ("vs_indirect", (6, 0, 0, 0, 0, 0), 2), ("hs_indirect", (0, 6, 0, 0, 0, 0), 2),
                 ("ds_indirect", (0, 0, 6, 0, 0, 0), 2), ("gs_indirect", (0, 0, 0, 4, 0, 0), 4),
                 ("ds_gs_indirect", (0, 0, 6, 4, 0, 0), 4))
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
        summary = "OWNED_D3D12_ATOMIC64_PRODUCERS_PASS cases=28 stages=VS,HS,DS,GS,AS,MS\n"
        trace = "".join(rows) + summary
        self.assertFalse(runner.atomic64_producers_failures(trace, ""))
        for invalid in (summary, trace.replace(rows[0], ""), trace + rows[0], trace + summary,
                        trace.replace("old=1234567800000000", "old=1234567700000000", 1),
                        trace.replace("cas=1234567800000001", "cas=1234567800000000", 1),
                        trace.replace("count=1", "count=2", 1), trace.replace("count=0", "count=1", 1),
                        trace.replace("cell=1234567800000009", "cell=1234567800000000", 1),
                        trace.replace("guard=cdcdcdcd", "guard=00000000", 1),
                        trace.replace("rgba=20000800", "rgba=10000400", 1),
                        trace.replace("rgba=20001400", "rgba=20000800", 1),
                        trace.replace("case=vs_indirect", "case=vs", 1)):
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
        for mode in range(3):
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
        lines.extend((
            prefix + "SPARSE iteration=0 tile=0 words=5555555500000000,5555555600000001,8765432100000001,"
                     "8765432100000001,123456789abcdef0,123456789abcdef0,2222222200000002,a11a11a11a11a11a,b22b22b22b22b22b",
            prefix + "SPARSE iteration=1 tile=1 words=5555555500000003,123456789abcdef0,5555555600000004,"
                     "5555555600000004,876543219abcdef0,876543219abcdef0,5555555600000004,a11a11a11a11a11a,b22b22b22b22b22b",
            prefix + "SPARSE iteration=2 tile=0 words=5555555500000006,5555555600000007,8765432100000007,"
                     "8765432100000007,123456789abcdef0,123456789abcdef0,5555555600000004,a11a11a11a11a11a,b22b22b22b22b22b",
            prefix + "SPARSE iteration=3 tile=0 words=5555555500000009,555555560000000a,876543210000000a,"
                     "876543210000000a,123456789abcdef0,123456789abcdef0,5555555600000004,a11a11a11a11a11a,b22b22b22b22b22b",
        ))
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
    def simulate(self, suite, trace, *, native=True, stderr=""):
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
                    kwargs["stderr"].write(stderr)
                    if native:
                        kwargs["stderr"].write(f"dyld[1]: {runtime}/lib/wine/x86_64-unix/winemetal.so\n")
                return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

            with patch.object(runner.subprocess, "run", side_effect=fake_run), \
                 contextlib.redirect_stdout(io.StringIO()):
                runner.run(args)
            return json.loads((args.out / "result.json").read_text())["pass"]

    def test_shader_review_rejects_missing_case_and_mismatched_readback(self):
        trace = "OWNED_D3D12_DXIL_RESOURCE_REVIEW_BACKEND id=owned-graphics-d3d12\n"
        for name, count in (("bounds", 44), ("gradients", 6), ("array-atomics", 24), ("bounds64", 25)):
            trace += "".join(f"OWNED_D3D12_DXIL_RESOURCE_REVIEW_WORD case={name} index={index} "
                             "actual=0x0000000000000000 expected=0x0000000000000000\n"
                             for index in range(count))
            trace += f"OWNED_D3D12_DXIL_RESOURCE_REVIEW_CASE_PASS case={name} checks={count}\n"
        trace += "OWNED_D3D12_DXIL_RESOURCE_REVIEW_PASS\n"
        self.assertTrue(self.simulate("dxil-resource-review", trace))
        self.assertFalse(self.simulate("dxil-resource-review", trace.replace(
            "OWNED_D3D12_DXIL_RESOURCE_REVIEW_CASE_PASS case=bounds64 checks=25\n", "")))
        self.assertFalse(self.simulate("dxil-resource-review", trace.replace(
            "OWNED_D3D12_DXIL_RESOURCE_REVIEW_WORD case=bounds64 index=24 "
            "actual=0x0000000000000000 expected=0x0000000000000000\n", "")))
        self.assertFalse(self.simulate("dxil-resource-review", trace.replace(
            "actual=0x0000000000000000", "actual=0x00000000ffffffff")))
        self.assertFalse(self.simulate("dxil-resource-review", trace, native=False))

    @staticmethod
    def writable_msaa_helper_control():
        # Controlled parser input, not GPU evidence. Each phase owns different
        # encoder/submission IDs so unrelated waits cannot satisfy its oracle.
        names = ("partial_array_helper_other_slice_clear", "unused_descriptor_helper_order")
        stdout = (f"OWNED_D3D12_WRITABLE_MSAA_HELPER_VALUES case={names[0]} helper=500 first=500 last=1283 clear=7,11,13,17\n"
                  f"OWNED_D3D12_WRITABLE_MSAA_HELPER_VALUES case={names[1]} prior=13579bdf helper=2468ace0 first=600 last=1399\n")
        lines = []
        stages = 31 | (1 << 27) | (1 << 28) | (1 << 29)
        for submission, name in enumerate(names, 101):
            lines.append(f"OWNED_D3D12_WRITABLE_MSAA_HELPER_PHASE begin={name}")

            def event(kind, **fields):
                lines.append("OWNED_GRAPHICS_TRACE " + json.dumps(dict(event=kind, submission=submission, **fields)))

            def compute(encoder, sync):
                if sync != "wait":
                    event("native_fence_wait_skipped", encoder=encoder, reason=sync, skip_count=1)
                event("native_compute_begin", encoder=encoder, begin_count=1)
                if sync == "wait":
                    event("native_compute_wait", encoder=encoder, wait_count=1)
                elif sync == "queue_barrier":
                    event("native_compute_queue_barrier", encoder=encoder, after=stages, before=1 << 27, barrier_count=1)
                event("native_compute_encode", encoder=encoder, encode_count=1)
                event("native_compute_update", encoder=encoder, update_count=1)
                event("native_compute_end", encoder=encoder, end_count=1)

            if submission == 101:
                compute(10, "wait")
                event("native_clear_begin", encoder=11, members=1, begin_count=1)
                event("native_clear_queue_barrier", encoder=11, after=stages, barrier_count=1)
                event("native_clear_load", encoder=11, aspect=4, clear_count=1)
                event("native_clear_update", encoder=11, update_count=1)
                event("native_clear_end", encoder=11, end_count=1)
            else:
                compute(20, "no_known_hazard")
                event("native_skipped_drain", barrier_count=1)
                compute(21, "wait")
                compute(22, "queue_barrier")
            event("native_skipped_drain", barrier_count=1)
            event("submit", queue=7)
            event("complete", queue=7, status=4)
            lines.append(f"OWNED_D3D12_WRITABLE_MSAA_HELPER_PHASE end={name}")
        return stdout, "\n".join(lines) + "\n"

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
        cases += ["partial_array_helper_other_slice_clear", "unused_descriptor_helper_order"]
        trace = ("OWNED_D3D12_WRITABLE_MSAA_BACKEND id=owned-graphics-d3d12\n"
                 "OWNED_D3D12_WRITABLE_MSAA_CAPS writable=1\n")
        trace += "".join(f"OWNED_D3D12_SM6_CASE_PASS {case}\n" for case in cases)
        trace += "".join(f"OWNED_D3D12_WRITABLE_MSAA_SHAPE samples={samples} array={array}\n"
                         for samples in (2, 4, 8) for array in (0, 1))
        values, stderr = self.writable_msaa_helper_control()
        trace += ("OWNED_D3D12_WRITABLE_MSAA_SCALAR type=sint\n"
                  "OWNED_D3D12_WRITABLE_MSAA_SCALAR type=float\n" + values + "OWNED_D3D12_WRITABLE_MSAA_PASS\n")
        self.assertTrue(self.simulate("writable-msaa", trace, stderr=stderr))
        self.assertFalse(self.simulate("writable-msaa", trace, native=False, stderr=stderr))
        for line in ("OWNED_D3D12_SM6_CASE_PASS out_of_bounds_sample_xy_layer\n",
                     "OWNED_D3D12_WRITABLE_MSAA_CAPS writable=1\n",
                     "OWNED_D3D12_SM6_CASE_PASS float_native_resolve_min_max\n",
                     "OWNED_D3D12_SM6_CASE_PASS typeless16_native_copy_float_srv\n",
                     "OWNED_D3D12_SM6_CASE_PASS partial_array_helper_other_slice_clear\n",
                     "OWNED_D3D12_SM6_CASE_PASS unused_descriptor_helper_order\n",
                     "OWNED_D3D12_WRITABLE_MSAA_SHAPE samples=8 array=1\n",
                     "OWNED_D3D12_WRITABLE_MSAA_SCALAR type=float\n"):
            with self.subTest(line=line):
                self.assertFalse(self.simulate("writable-msaa", trace.replace(line, "", 1), stderr=stderr))
        for extra in ("OWNED_D3D12_SM6_CASE_PASS same_dispatch\n",
                      "OWNED_D3D12_SM6_MISMATCH invalid samples\n",
                      "OWNED_D3D12_WRITABLE_MSAA_PASS\n"):
            with self.subTest(extra=extra):
                self.assertFalse(self.simulate("writable-msaa", trace + extra, stderr=stderr))
        self.assertFalse(self.simulate("writable-msaa", trace))

    def test_writable_msaa_helper_order_requires_own_waits_and_fragment_consumers(self):
        stdout, stderr = self.writable_msaa_helper_control()
        self.assertEqual(runner.writable_msaa_failures(stdout, stderr), [])
        for old, new in (("helper=500", "helper=501"), ("last=1283", "last=1282"),
                         ("clear=7,11,13,17", "clear=7,11,13,16"),
                         ("prior=13579bdf", "prior=2468ace0"), ("helper=2468ace0", "helper=13579bdf"),
                         ("first=600", "first=601"), ("last=1399", "last=1398")):
            with self.subTest(numeric=old):
                self.assertTrue(runner.writable_msaa_failures(stdout.replace(old, new), stderr))
        lines = stderr.splitlines()
        required = []
        for line in lines:
            if not line.startswith("OWNED_GRAPHICS_TRACE "):
                continue
            event = json.loads(line.split("OWNED_GRAPHICS_TRACE ", 1)[1])
            if (event["event"] in ("native_compute_wait", "native_compute_queue_barrier",
                                   "native_clear_queue_barrier", "submit", "complete")
                    or (event["event"] == "native_fence_wait_skipped" and event.get("encoder") == 20)):
                required.append(line)
        for line in required:
            with self.subTest(missing=line):
                self.assertTrue(runner.writable_msaa_failures(stdout, stderr.replace(line + "\n", "", 1)))
        helper_wait = next(line for line in lines if '"event": "native_compute_wait"' in line and '"encoder": 21' in line)
        consumer_barrier = next(line for line in lines if '"event": "native_compute_queue_barrier"' in line)
        clear_barrier = next(line for line in lines if '"event": "native_clear_queue_barrier"' in line)
        drain = next(index for index, line in enumerate(lines) if '"event": "native_skipped_drain"' in line and '"submission": 102' in line)
        early_drain = lines.copy()
        moved = early_drain.pop(drain)
        early_drain.insert(next(index for index, line in enumerate(early_drain) if '"encoder": 20' in line), moved)
        mutations = [
            stderr.replace(helper_wait, helper_wait.replace('"encoder": 21', '"encoder": 20')),
            stderr.replace(helper_wait, helper_wait.replace('"submission": 102', '"submission": 101')),
            stderr.replace(consumer_barrier, consumer_barrier.replace('"encoder": 22', '"encoder": 21')),
            stderr.replace(clear_barrier, clear_barrier.replace('"encoder": 11', '"encoder": 10')),
            stderr.replace('"encoder": 11', '"batch": 0'),
            stderr.replace('"after": 939524127', '"after": 939524096'),
            "\n".join(lines[:drain] + lines[drain + 1:]),
            "\n".join(early_drain),
            stderr.replace('"status": 4', '"status": 5'),
            stderr.replace('"queue": 7, "status"', '"queue": 8, "status"'),
            stderr.replace("OWNED_GRAPHICS_TRACE {", "OWNED_GRAPHICS_TRACE [", 1),
            stderr.replace("OWNED_GRAPHICS_TRACE " + json.dumps(dict(event="native_compute_begin", submission=101, encoder=10, begin_count=1)),
                           "OWNED_GRAPHICS_TRACE []"),
            stderr.replace("HELPER_PHASE end=", "HELPER_PHASE begin=", 1),
            stderr + "\n" + stderr,
            "",
        ]
        for altered in mutations:
            with self.subTest(trace=altered):
                self.assertTrue(runner.writable_msaa_failures(stdout, altered))

    def test_mapping_requires_live_control_view_and_no_growth(self):
        trace = ("OWNED_D3D12_ARENA_HEAP_BACKEND id=owned-graphics-d3d12\n"
                 "OWNED_D3D12_ARENA_HEAP_MAPPING_PASS failures=32 views_before=1 views_after=1\n")
        self.assertTrue(self.simulate("arena-heap-mapping", trace))
        self.assertFalse(self.simulate("arena-heap-mapping", trace.replace("views_after=1", "views_after=33")))
        self.assertFalse(self.simulate("arena-heap-mapping", trace.replace("views_before=1 views_after=1",
                                                                          "views_before=0 views_after=0")))


class FsrPresentationEvidence(unittest.TestCase):
    def test_native_pixels_and_original_sdk_output_sequence(self):
        # CPU-only parser protocol, deliberately NOT GPU/native display proof.
        spec = importlib.util.spec_from_file_location("fsr_capture_checker",
            ROOT / "graphics-translator/tests/check-owned-fg-capture.py")
        checker = importlib.util.module_from_spec(spec); spec.loader.exec_module(checker)
        with tempfile.TemporaryDirectory(dir=SCRATCH, prefix="fsr-present-") as directory:
            base = Path(directory); fixture = base / "fixture"; captures = base / "fg-capture"
            fixture.mkdir(); captures.mkdir()
            stdout = "OWNED_D3D12_FSR_FG_PASS\n"
            stderr = "OWNED_D3D12_FSR_FG_OBSERVER_READY\n"
            expected = [(1, 1, 1), (1, 0, 0), (2, 1, 1), (2, 0, 0), (3, 1, 1), (3, 0, 0),
                        (4, 1, 1), (4, 0, 0), (5, 0, 0), (6, 0, 0), (7, 1, 1), (7, 0, 0),
                        (8, 1, 1), (9, 0, 0), (10, 1, 1), (10, 0, 0), (11, 1, 1), (11, 0, 0), (12, 1, 1)]
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
            for stream, frames in (("callback", list(range(1, 13))), ("builtin_ui", list(range(11, 15)))):
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
            args.stderr.write_text(stderr.replace("OWNED_D3D12_FSR_FG_DRAWABLE sequence=2 ",
                "dyld[123]: /System/Library/StateReporting.framework/VersiOWNED_D3D12_FSR_FG_DRAWABLE sequence=2 "))
            self.assertEqual(checker.check(args)["attempted_drawables"], 23)
            args.stderr.write_text(stderr)
            args.stdout.write_text(stdout.replace("frame=1 generated=0 output_index=0", "frame=1 generated=1 output_index=1"))
            with self.assertRaisesRegex(ValueError, "sequence mismatch"): checker.check(args)
            args.stdout.write_text(stdout)
            bad = bytearray((fixture / "fg-callback-3-1-1.bin").read_bytes())
            bad[128 * 20 * 4:128 * 21 * 4] = bytes((19, 19, 19, 255)) * 128
            (captures / "drawable-5.bin").write_bytes(bad)
            with self.assertRaisesRegex(ValueError, "content mismatch"): checker.check(args)
            (captures / "drawable-5.bin").write_bytes((fixture / "fg-callback-3-1-1.bin").read_bytes())
            args.stderr.write_text(stderr.replace("sequence=5 drawable=d5 time=5", "sequence=5 drawable=d5 time=0"))
            with self.assertRaisesRegex(ValueError, "physical output proof incomplete"): checker.check(args)
            args.stderr.write_text(stderr.replace("sequence=1 drawable=d1 time=1", "sequence=1 drawable=d1 time=0"))
            # A bootstrap skip is explicit, but every selected native generated output,
            # critical real/error output and UI proof remains exercised.
            result = checker.check(args)
            self.assertEqual(result["positively_displayed_drawables"], 22)
            self.assertEqual(result["skipped_drawables"], 1)
            self.assertEqual(len(result["physically_verified_generated_outputs"]), 2)
            args.stderr.write_text(stderr)
            duplicate = bytearray((fixture / "fg-callback-3-0-1.bin").read_bytes())
            marker = bytes((35, 190, 220, 255))
            for y in range(8): duplicate[y * 128 * 4:y * 128 * 4 + 32] = marker * 8
            (fixture / "fg-callback-3-1-1.bin").write_bytes(duplicate)
            (captures / "drawable-5.bin").write_bytes(duplicate)
            with self.assertRaisesRegex(ValueError, "repeats current color"): checker.check(args)


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
            summary = prefix + "_PASS frames=6 drawables=9 generated=3 bootstrap_not_presented=3 ngx_feature=11 hudless=1 ui=current_backbuffer history=native reset=fresh_equivalent release_before_execute=1 replay=native_encode caller_owned_present=1\n"
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
                    stdout += f"{prefix}_FRAME frame={frame} generated={generated} generation=native_metalfx width=128 height=128 submitted_present={sequence} statistics=actual result=0x00000000 actual_present={sequence - 1} sync_qpc={sequence * 100} present_refresh={sequence} sync_refresh={sequence} pace_qpc_start={sequence * 100 - 10} qpc_start={sequence * 100} qpc_end={sequence * 100 + 10} frequency=1000 source=ngx11_gpu_copy_to_dxgi\n"
                    stderr += f"{prefix}_DRAWABLE sequence={sequence} drawable={sequence} width=128 height=128 format=70 path=drawable-{sequence:06}.bin\n"
                    stderr += f"{prefix}_PRESENTED sequence={sequence} drawable={sequence} time={1 + sequence / 60:.9f}\n"
                if frame in (1, 5, 6):
                    (fixture / f"fg-ngx-{frame}-1.bin").write_bytes(pixels)
                    stdout += f"OWNED_D3D12_NGX_BOOTSTRAP_OUTPUT frame={frame} reset=1 output_written=1 presented=0 policy=caller_no_history\n"
                if frame in (4, 5): (fixture / f"fg-ngx-fresh-{frame}.bin").write_bytes(pixels)
                if frame == 6: (fixture / "fg-ngx-replay-6.bin").write_bytes(pixels)
            stdout += f"{prefix}_DRAIN submitted_present=11 statistics=actual result=0x00000000 actual_present=10 sync_qpc=1100 present_refresh=11 sync_refresh=11 qpc_end=1150 frequency=1000 output_vblanks=3\n"
            args.stdout.write_text(stdout); args.stderr.write_text(stderr)
            result = checker.check(args)
            self.assertTrue(result["pass"])
            self.assertEqual(result["attempted_drawables"], 9)
            self.assertEqual(result["positively_displayed_drawables"], 9)
            self.assertEqual(len(result["caller_bootstrap_outputs_not_presented"]), 3)
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
            for broken, reason in ((stdout.replace("generation=native_metalfx", "generation=copy_current", 1), "native source"),
                                   (stdout.replace("presented=0 policy=caller_no_history", "presented=1 policy=caller_no_history", 1), "bootstrap presentation policy"),
                                   (stdout + summary, "consumer incomplete")):
                args.stdout.write_text(broken)
                with self.assertRaisesRegex(ValueError, reason): checker.check(args)
            args.stdout.write_text(stdout.replace(summary, prefix + "_PASS caller_owned_present=1 replay=native_encode release_before_execute=1 hudless=1 ngx_feature=11 bootstrap_not_presented=3 generated=3 drawables=9 frames=6\n"))
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
            public = fixture / "fg-ngx-1-1.bin"; original = public.read_bytes(); public.write_bytes(sentinel)
            with self.assertRaisesRegex(ValueError, "native bootstrap output was not written"): checker.check(args)
            public.write_bytes(original)
            public = fixture / "fg-ngx-3-1.bin"; original = public.read_bytes()
            public.write_bytes((fixture / "fg-ngx-3-0.bin").read_bytes())
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
                    "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_fg count_max=1 unsupported_mfg=1 disable_buffer=gpu_history_hint reset=native_history release_before_execute=1 replay=native_encode\n"
                    "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_fg_camera_motion ortho=0 frames=6 independent_features=3 equivalent_motion=1 nonzero_effect=1 release_before_execute=1\n"
                    "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_fg_camera_motion ortho=1 frames=6 independent_features=3 equivalent_motion=1 nonzero_effect=1 release_before_execute=1\n"
                    "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_fg_ui premultiplied=1 alpha_only=1 independent_ui_origin=1 invalid_inputs=1 release_before_execute=1 replay=1\n")
        native = "".join(f"OWNED_D3D12_NGX_NATIVE frame={frame} reset={int(frame in (0, 5))} replay={replay} output_written=1 different_current={int(frame in (3, 4) and not replay)} resources_released=1 consumer=gpu_readback frame_id={frame if frame < 7 else 9} not_rendering={int(frame == 6)}\n"
                         for frame in range(9) for replay in range(2 if frame in (0, 7, 8) else 1))
        rejections = "".join(f"OWNED_D3D12_NGX_REJECT case={kind} {kind}={value} result=unsupported_parameter\n"
                             for kind in ("count", "index") for value in (2, 3, 4))
        rejections += "OWNED_D3D12_NGX_REJECT case=disable_buffer result=invalid_parameter texture=1 non_uav=1 too_small=1 cross_device=1 output_untouched=1 zero_count_index=invalid_parameter\n"
        rejections += "".join(f"OWNED_D3D12_NGX_REJECT case=camera_matrix ortho={o} missing=missing_input nan=invalid_parameter infinity=invalid_parameter singular=invalid_parameter output_untouched=1\n" for o in (0, 1))
        rejections += "OWNED_D3D12_NGX_REJECT case=ortho_camera missing_matrix=missing_input missing_basis=missing_input nan=invalid_parameter infinity=invalid_parameter singular=invalid_parameter outputs_untouched=1\n"
        native += "".join(f"OWNED_D3D12_NGX_DISABLE_HINT frame={f} replay={r} value={int(f == 0 or f >= 5 or r != 0)} before_execute_untouched=1 after_fence=1 tail_untouched=1 history_hint=1\n" for f in range(9) for r in range(2 if f in (0, 7, 8) else 1))
        native += "".join(f"OWNED_D3D12_NGX_CAMERA_MOTION ortho={o} frame={f} max_delta=1 different_zero={int(f > 0)} row_major=1 translated_last_row=1 matrix_snapshotted=1\n" for o in (0, 1) for f in range(6))
        native += "".join(f"OWNED_D3D12_NGX_INPUT_PASS case=camera infinite_far={i} frames=6 independent_features=4 inverse_projection=1 invalid_untouched=1 native_boundaries=1 explicit_projection=1 complete_basis=1 aspect=2 generated_beyond_warmup=1 release_before_execute=1 snapshot=1\n" for i in (0, 1))
        native += "".join(f"OWNED_D3D12_NGX_INPUT_PASS case=motion invalid_sentinel={i} frames=6 independent_features=5 equivalent_motion=1 poisoned_padding=1 output_real_rects=1 jitter_or_inverse=1 generated_beyond_warmup=1 release_before_execute=1 snapshot=1\n" for i in (0, 1))
        native += "".join(f"OWNED_D3D12_NGX_INPUT_REJECT case=camera infinite_far={i} aspect_zero_negative_nan_infinity=invalid_parameter far_negative_infinity_nan=invalid_parameter outputs_untouched=1\n" for i in (0, 1))
        scene = "OWNED_D3D12_NGX_SCENE frames=14 reset=1 output_written=1 actual_count=unavailable consumer=gpu_readback\n"
        self.ngx_fg = self.ngx + rejections + native + scene + "OWNED_D3D12_NGX_FG_PASS capacity=1 native_output=1 unsupported_mfg=1\n"
        self.ngx += "".join(f"OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_rr variant={v} frames=8 handle13=1 denoising_guides=1 matrices_snapshotted=1 release_before_execute=1 replay=1 oracle_artifacts=8\n" for v in range(8))
        self.ngx += "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_rr_responsivity changed_rgb=42 origins=1 invalid_resource_subrect=1 release_before_execute=1 replay=1\n"
        self.ngx += "OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_alpha flag128=1 nonopaque_contrast=1 rgb_preserved=1\n"
        self.ngx += rejections + native + scene + "OWNED_D3D12_NGX_FEATURES_PASS\n"
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
            ("ngx-fg", self.ngx_fg, "different_current=1", "different_current=0"),
            ("ngx-fg", self.ngx_fg, "output_real_rects=1", "output_real_rects=0"),
            ("ngx-fg", self.ngx_fg, "frame=0 reset=1", "frame=0 reset=0"),
            ("ngx-fg", self.ngx_fg, "resources_released=1", "resources_released=0"),
            ("ngx-fg", self.ngx_fg, "output_untouched=1", "output_untouched=0"),
            ("ngx-fg", self.ngx_fg, "case=count count=2 result=unsupported_parameter", "case=count count=2 result=success"),
            ("ngx-fg", self.ngx_fg, "frames=14 reset=1", "frames=14 reset=0"),
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
        indices = [index for index, line in enumerate(lines) if line.startswith("OWNED_D3D12_NGX_NATIVE ")]
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


class RefitChainEvidence(unittest.TestCase):
    """Controlled public-layout parser inputs; these are not GPU evidence."""

    @staticmethod
    def stimulus(indexed=False):
        prefix = "OWNED_D3D12_REFIT_CHAIN"
        cases = (("chain", (1, 2)), ("replay", (1, 2)), ("cross-queue", (3,)), ("after-cross", (1, 2)))
        bits = lambda value: struct.unpack("<I", struct.pack("<f", value))[0]
        words = lambda values: ",".join(f"{value:08x}" for value in values)
        lines = [prefix + "_SOURCE blas_address=0000000000001000 tlas_address=0000000000002000 "
                 "blas_prebuild=1280 tlas_prebuild=1280",
                 prefix + "_RECORDINGS initial_lists=2 r_executions=3 r2_executions=1 "
                 f"unpublished_builds=2 cross_queue=1 indexed={int(indexed)}"]
        for name, steps in cases:
            for step in steps:
                for lane in range(4):
                    instance, geometry = divmod(lane, 2)
                    values = [1, 0, 0x80 + 16 * step + instance, instance, geometry, 0xff,
                              bits(3 * step + 1), 0, bits(0.25), bits(0.25), 0x1000, 0xffffffff, 0x101]
                    lines.append(f"{prefix}_RAY case={name} step={step} lane={lane} words={words(values)}")
            for kind in ("blas", "tlas"):
                visual, serial, pointers = ((328, 440, 0) if indexed else (296, 408, 0)) \
                    if kind == "blas" else (136, 264, 2)
                lines.append(f"{prefix}_SIZE case={name} kind={kind} compacted=1280 visualization={visual} "
                             f"serialized={serial} pointers={pointers} current=1280 header_serialized={serial} "
                             f"deserialized=1280 header_pointers={pointers} identifier=1")
                if kind == "blas":
                    values = [bits(value) for g in range(2) for value in
                              (4 * g, 0, 2 * steps[-1], 4 * g + 1, 0, 2 * steps[-1], 4 * g, 1, 2 * steps[-1])]
                else:
                    values = []
                    for instance in range(2):
                        values.extend(bits(value) for value in
                                      (1, 0, 0, 0, 0, 1, 0, 4 * instance, 0, 0, 1, steps[-1]))
                        values.extend((0xff000080 + 16 * steps[-1] + instance, 0, 0x1000, 0))
                lines.append(f"{prefix}_SHADOW case={name} kind={kind} type={int(kind == 'blas')} "
                             f"descriptors=2 descriptor_mismatches=0 words={words(values)}")
            lines.append(f"{prefix}_CASE case={name} steps={len(steps)} status=PASS")
        lines.extend((prefix + "_RESULT cases=4 refits=14 ray_lanes=28 shadow_snapshots=8 unpublished_builds=2 failures=0",
                      prefix + "_PASS"))
        stderr = "\n".join(f"{prefix}_EXECUTE case={name} {boundary}" for name in
                           ("chain", "replay", "cross-return") for boundary in ("begin", "end"))
        return "\n".join(lines), stderr

    def test_refit_indexed_and_default_modes_have_distinct_exact_sizes(self):
        for indexed in (False, True):
            with self.subTest(indexed=indexed):
                stdout, stderr = self.stimulus(indexed)
                self.assertEqual(runner.raytracing_refit_chain_failures(stdout, stderr), [])
        stdout, stderr = self.stimulus(False)
        self.assertEqual(runner.raytracing_refit_chain_failures(stdout.replace(" indexed=0", ""), stderr), [])

    def test_refit_wrong_mode_or_mixed_sizes_cannot_pass(self):
        plain, stderr = self.stimulus(False)
        indexed, _ = self.stimulus(True)
        corruptions = (
            plain.replace(" indexed=0", " indexed=1"),
            indexed.replace(" indexed=1", " indexed=0"),
            indexed.replace(" indexed=1", ""),
            indexed.replace("visualization=328", "visualization=296", 1),
            indexed.replace("serialized=440", "serialized=408", 1),
            plain.replace("visualization=296", "visualization=328", 1),
            indexed.replace("descriptor_mismatches=0", "descriptor_mismatches=1", 1),
        )
        for number, stdout in enumerate(corruptions):
            with self.subTest(number=number):
                self.assertTrue(runner.raytracing_refit_chain_failures(stdout, stderr))

    def test_refit_mode_marker_must_be_unique_and_well_formed(self):
        stdout, stderr = self.stimulus(True)
        marker = next(line for line in stdout.splitlines() if "_RECORDINGS " in line)
        for replacement in ("", marker + "\n" + marker,
                            marker + "\n" + marker.replace("indexed=1", "indexed=0"),
                            marker.replace("indexed=1", "indexed=2"),
                            marker.replace("indexed=1", "indexed=01"), marker + " extra=1"):
            with self.subTest(replacement=replacement):
                self.assertTrue(runner.raytracing_refit_chain_failures(stdout.replace(marker, replacement), stderr))


class RTExecutableCacheEvidence(unittest.TestCase):
    def test_real_native_link_counts_and_live_clear_required(self):
        names=("concurrent-cold","warm","local-root-variant","clear-live")
        cases=((4,4),(1,1),(2,2),(1,3))
        output="".join(f"OWNED_D3D12_RT_EXECUTABLE_CASE case={name} objects={objects} gpu_dispatches={dispatches} output=exact\n"
            for name,(objects,dispatches) in zip(names,cases))
        output+="OWNED_D3D12_RT_EXECUTABLE_PASS cases=4 gpu_dispatches=10 mutable_tables_and_localroots=isolated\n"
        groups={"concurrent-cold":[("pipeline_artifact_operation","a")],"warm":[("pipeline_artifact_hit","a")],
                "local-root-variant":[("pipeline_artifact_operation","b"),("pipeline_artifact_hit","b")],
                "clear-live":[("pipeline_artifact_operation","c")]}
        def log(values):
            rows=[];index=1
            for name in names:
                rows.append(f"OWNED_D3D12_RT_EXECUTABLE_PHASE begin={name}")
                for event,key in values[name]:
                    rows.append("OWNED_GRAPHICS_TRACE "+json.dumps(dict(event=event,event_id=index,kind="native_rt_executable",key=key)));index+=1
                rows.append(f"OWNED_D3D12_RT_EXECUTABLE_PHASE end={name}")
            for event in ("submit","complete"):
                rows.append("OWNED_GRAPHICS_TRACE "+json.dumps(dict(event=event,event_id=index,queue=1,submission=1,status=4)));index+=1
            return "\n".join(rows)+"\n"
        self.assertTrue(runner.rt_executable_output_matches(output,log(groups),True,True))
        self.assertFalse(runner.rt_executable_output_matches(output,log(groups),False,True))
        for name in ("concurrent-cold","local-root-variant","clear-live"):
            broken={key:list(value) for key,value in groups.items()};broken[name]=[]
            self.assertFalse(runner.rt_executable_output_matches(output,log(broken),True,True))
        broken={key:list(value) for key,value in groups.items()};broken["warm"].append(("pipeline_artifact_operation","a"))
        self.assertFalse(runner.rt_executable_output_matches(output,log(broken),True,True))
        broken={key:list(value) for key,value in groups.items()};broken["local-root-variant"]=[("pipeline_artifact_operation","a")]
        self.assertFalse(runner.rt_executable_output_matches(output,log(broken),True,True))
        self.assertFalse(runner.rt_executable_output_matches(output+"OWNED_D3D12_RAYTRACING_CASE_MISMATCH local_root\n",log(groups),True,True))


class SubmissionFoundationEvidence(unittest.TestCase):
    def test_actual_resolved_execution_and_reuse_required(self):
        names=["volatile_replay","descriptor_cache_reuse","retired_icb_reuse","placed_alias_clears",
               "placed_disjoint_ranges","placed_sparse_range_replay","query_fanout","sparse_mapping_positive","cpu_strip_transfer",
               "dispatch_independence"]
        output="".join(f"OWNED_D3D12_SUBMISSION_FOUNDATION_CASE_PASS case={name} proof=exact\n" for name in names)
        output=output.replace("case=placed_disjoint_ranges proof=exact", "case=placed_disjoint_ranges textures=2 buffer=1 pixels=185 words=64 exact=1")
        output=output.replace("case=placed_sparse_range_replay proof=exact", "case=placed_sparse_range_replay executions=4 close_count=1 mapping=unmapped,disjoint,overlap,null pixels=64 words=256 exact=1")
        output=output.replace("case=dispatch_independence proof=exact",
            "case=dispatch_independence phases=9 table_replays=4 potential_raw_war_waw=serial "
            "budget=65 roots=8 external_alias=serial all_words_and_guards=exact")
        output+="OWNED_D3D12_SUBMISSION_FOUNDATION_PASS cases=10\n"
        records=[dict(event="recording_plan_sealed",passes=3,descriptor_values_captured=0),
            dict(event="residency_add_batch",allocations=5,commits=1)]
        records += [dict(event="indirect_buffer_reuse",count=1,capacity=2) for _ in range(2)]
        records += [dict(event="sparse_publication_elided",proof="no_sparse_heap_or_opaque_access") for _ in range(4)]
        records += [dict(event="submission_access_opaque",reason="sparse_alias")]
        records += [dict(event="native_compute_begin",reason="dispatch_external_backing",dispatches=3,concurrent=0)]
        records += [dict(event="query_fanout_batch",targets=2,native_encoders=1,reset=0),
                    dict(event="query_fanout_batch",targets=1,native_encoders=1,reset=0),
                    dict(event="submit",queue=1,submission=1),dict(event="complete",queue=1,submission=1,status=4)]
        for name,hit in [("cold",False),("payload",True),("unrelated",True),("copy",False),
                         ("copy_payload",True),("null",False),("null_replay",True),("restore",False),
                         ("va_epoch",False),("va_replay",True)]:
            records.append(dict(event="submission_plan_work",_phase="descriptor_cache_"+name,
                descriptor_cache_hits=int(hit),descriptor_cache_misses=int(not hit),
                descriptor_cache_builds=int(not hit),descriptor_resolutions=int(not hit)))
        for ordinal,name in enumerate(("placed_alias_clears","placed_disjoint_ranges"),1):
            rows=[dict(event="execute_list",queue=2,list=10+ordinal,recording=1,submission=ordinal)]
            rows += [dict(event="submission_heap_clear_in_place",encoder=100*ordinal+i) for i in (1,2)]
            for i in (1,2):
                rows += [dict(event="native_clear_begin",encoder=100*ordinal+i,members=1,begin_count=1),
                         dict(event="native_clear_end",encoder=100*ordinal+i,end_count=1)]
            if name=="placed_disjoint_ranges":
                rows += [dict(event="native_fence_wait_skipped",encoder=100*ordinal+i,
                              reason="no_known_hazard",skip_count=1) for i in (1,2)]
            rows += [dict(event="submit",queue=2,submission=ordinal),
                     dict(event="complete",queue=2,submission=ordinal,status=4)]
            records += [dict(row,_phase=name) for row in rows]
        records.append(dict(event="recording_plan_sealed",list=20,recording=7,passes=7,descriptor_values_captured=0))
        for ordinal,name in enumerate(("placed_sparse_unmapped","placed_sparse_disjoint","placed_sparse_overlap","placed_sparse_null"),1):
            required=name=="placed_sparse_overlap"
            fields=dict(queue=3,list=20,mapping_generation=50+ordinal,heap_ranges=4)
            rows=[dict(event="execute_list",queue=3,list=20,recording=7,submission=ordinal),
                  dict(event="sparse_publication_proof",required=required,current=not required,**fields)]
            if required:
                rows += [dict(event="sparse_publication_required",reason="mapped_heap_or_opaque_access",**fields),
                         dict(event="sparse_alias_flush",queue=3,value=ordinal)]
            else:
                rows += [dict(event="sparse_publication_elided",proof="disjoint_sparse_heap_ranges",**fields)]
                rows += [dict(event="sparse_barrier_elided",encoder=300+i,**fields) for i in range(4)]
            rows += [dict(event="submit",queue=3,submission=ordinal),
                     dict(event="complete",queue=3,submission=ordinal,status=4)]
            records += [dict(row,_phase=name) for row in rows]
        def check(rows,proof=True,text=output):
            lines=[];active=None
            for i,row in enumerate(rows):
                record=dict(row,event_id=i+1);phase=record.pop("_phase",None)
                if phase!=active:
                    if active:lines.append("OWNED_D3D12_SUBMISSION_FOUNDATION_PHASE end="+active+"\n")
                    if phase:lines.append("OWNED_D3D12_SUBMISSION_FOUNDATION_PHASE begin="+phase+"\n")
                    active=phase
                lines.append("OWNED_GRAPHICS_TRACE "+json.dumps(record)+"\n")
            if active:lines.append("OWNED_D3D12_SUBMISSION_FOUNDATION_PHASE end="+active+"\n")
            log="".join(lines)
            return runner.submission_foundation_output_matches(text,log,proof,True)
        self.assertTrue(check(records));self.assertFalse(check(records,False))
        self.assertFalse(check(records,text=output.replace("dispatch_independence", "missing_dispatch_independence")))
        for kind in ("recording_plan_sealed","residency_add_batch","indirect_buffer_reuse","sparse_publication_elided",
                     "submission_heap_clear_in_place","native_clear_begin","native_clear_end",
                     "query_fanout_batch","submission_plan_work","native_compute_begin",
                     "execute_list","native_fence_wait_skipped","sparse_publication_proof",
                     "sparse_publication_required","sparse_alias_flush","sparse_barrier_elided"):
            self.assertFalse(check([row for row in records if row["event"]!=kind]))
        changed=[dict(row) for row in records];changed[0]["descriptor_values_captured"]=1
        self.assertFalse(check(changed))
        changed=[dict(row) for row in records]
        for row in changed:
            if row["event"]=="native_clear_begin":row["members"]=2
        self.assertFalse(check(changed))
        changed=[dict(row) for row in records]
        for row in changed:
            if row["event"]=="query_fanout_batch":row["native_encoders"]=2
        self.assertFalse(check(changed))
        self.assertFalse(check(records,text=output+"OWNED_D3D12_SUBMISSION_FOUNDATION_CASE_PASS case=query_fanout duplicate=1\n"))
        changed=[dict(row) for row in records]
        for row in changed:
            if row.get("_phase")=="descriptor_cache_payload":row["descriptor_resolutions"]=1
        self.assertFalse(check(changed))
        changed=[dict(row) for row in records]
        for row in changed:
            if row.get("_phase")=="descriptor_cache_copy":
                row.update(descriptor_cache_hits=1,descriptor_cache_misses=0,descriptor_cache_builds=0,descriptor_resolutions=0)
        self.assertFalse(check(changed))
        for phase,kind,field,value in (
            ("placed_disjoint_ranges","native_fence_wait_skipped","reason","queue_barrier"),
            ("placed_sparse_disjoint","sparse_publication_proof","current",0),
            ("placed_sparse_disjoint","sparse_publication_proof","mapping_generation",51),
            ("placed_sparse_overlap","sparse_publication_proof","required",False),
            ("placed_sparse_overlap","sparse_publication_proof","current",True),
            ("placed_sparse_null","sparse_publication_proof","heap_ranges",0),
            ("placed_sparse_disjoint","sparse_barrier_elided","mapping_generation",1),
            ("placed_sparse_null","execute_list","recording",8),
            ("placed_sparse_overlap","sparse_publication_required","reason","stale"),
            ("placed_sparse_overlap","complete","status",5),
        ):
            changed=[dict(row) for row in records]
            for row in changed:
                if row.get("_phase")==phase and row["event"]==kind:row[field]=value
            self.assertFalse(check(changed),(phase,kind,field))
        changed=[dict(row) for row in records]
        target=next(i for i,row in enumerate(changed) if row.get("_phase")=="placed_sparse_disjoint" and row["event"]=="sparse_publication_proof")
        changed.insert(target,dict(changed[target]))
        self.assertFalse(check(changed))
        changed=[dict(row) for row in records]
        target=next(i for i,row in enumerate(changed) if row.get("_phase")=="placed_sparse_disjoint" and row["event"]=="execute_list")
        mapping_flush=dict(event="sparse_alias_flush",queue=3,value=90,_phase="placed_sparse_disjoint")
        changed.insert(target,mapping_flush)
        self.assertTrue(check(changed))  # A preceding mapping task is outside the consumer's proof.
        changed.pop(target);changed.insert(target+1,mapping_flush)
        self.assertFalse(check(changed))
        self.assertFalse(check(records,text=output.replace("pixels=185","pixels=184")))


class WorkGraphBatchEvidence(unittest.TestCase):
    def test_full_groups_coordinates_returns_and_native_batches_required(self):
        output="".join(f"OWNED_D3D12_WG_BATCH_CASE atomic={a} gpu_input={g} groups={n} count={n} guard=ok coordinates_or_returns=exact\n"
            for a,g,n in [(0,0,66049),(0,1,66049),(1,0,65),(1,1,65)])
        output+="OWNED_D3D12_WG_BATCH_PASS cases=4\n"
        expected=[(0,65535),(65535,514)]*2+[(0,32),(32,32),(64,1)]*2
        events=[dict(event="work_graph_node_batch",event_id=i+1,first_group=first,groups=count,terminal=1)
                for i,(first,count) in enumerate(expected)]
        events += [dict(event="submit",event_id=11,queue=1,submission=1),
                   dict(event="complete",event_id=12,queue=1,submission=1,status=4)]
        def trace(rows):return "".join("OWNED_GRAPHICS_TRACE "+json.dumps(row)+"\n" for row in rows)
        check=lambda out,rows,proof=True:runner.workgraph_batches_output_matches(out,trace(rows),proof,True)
        self.assertTrue(check(output,events))
        self.assertFalse(check(output,events,False))
        self.assertFalse(check(output.replace("count=66049","count=66048",1),events))
        self.assertFalse(check(output.replace("guard=ok","guard=bad",1),events))
        self.assertFalse(check(output,events[:-1]))
        altered=[dict(row) for row in events];altered[2]["groups"]=1
        self.assertFalse(check(output,altered))
        altered=[dict(row) for row in events];altered[6]["terminal"]=0
        self.assertFalse(check(output,altered))


class WideWaveFocusedEvidence(unittest.TestCase):
    def test_all_partial_integer_and_legacy_wide_cases_required(self):
        lines=["OWNED_D3D12_SM6_BACKEND id=owned-graphics-d3d12",
            "OWNED_D3D12_SM6_CAPS shader_model=0x66/0x0 wave=1 lanes=4-32 total_lanes=2560"]
        lines += ["OWNED_D3D12_SM6_CASE_PASS "+case for case in runner.WIDE_WAVE_CASES]
        lines += [f"OWNED_D3D12_WAVES_REJECT entry={entry} hr=0x80004005" for entry in runner.SM6_REJECTS]
        lines += ["OWNED_D3D12_WIDE_WAVES_PASS"]
        out="\n".join(lines)+"\n"
        check=lambda text,proof=True:runner.wide_waves_output_matches(text,"",proof,True)[0]
        self.assertTrue(check(out))
        self.assertFalse(check(out,False))
        self.assertEqual(runner.BACKEND_PREFIXES["wide-waves"],"OWNED_D3D12_SM6")
        for case in runner.WIDE_WAVE_CASES:
            marker="OWNED_D3D12_SM6_CASE_PASS "+case+"\n"
            self.assertFalse(check(out.replace(marker,"")))
            self.assertFalse(check(out+marker))
        self.assertFalse(check(out+"OWNED_D3D12_SM6_MISMATCH integer\n"))
        self.assertFalse(check(out.replace("hr=0x80004005","hr=0x00000000",1)))


class TemporalFailures(unittest.TestCase):
    def test_sampled_sr_must_observe_native_retirement_counters(self):
        row = ("D3DALLOY_SR_GPU_PHASE sample=6 generation=1 input=624x360 capacity=624x360 "
               "output=1280x720 sharpen=0 reset=0 slots=12 prefix_fold=0 direct_center=0 "
               "pre_us=0.000 native_us=615.042 alpha_us=0.000 finish_us=0.000 copy_us=0.000")
        samples, errors = runner.sr_timing_evidence(row, 3)
        self.assertEqual(errors, [])
        self.assertEqual((samples[0]["sample"], samples[0]["native_us"]), (6, 615.042))
        for broken in ("", row.replace("sample=6", "sample=5"),
                       row.replace("slots=12", "slots=8"), row.replace("pre_us=0.000", "pre_us=1.000"),
                       row.replace("615.042", "nan")):
            with self.subTest(broken=broken):
                self.assertTrue(runner.sr_timing_evidence(broken, 3)[1])
        for suite, period in (("smoke", 1), ("temporal", 0), ("sr-alpha", 1 << 32)):
            with self.subTest(suite=suite, period=period), self.assertRaisesRegex(ValueError, "SR suite"):
                runner.run(argparse.Namespace(suite=suite, sr_gpu_timing_period=period))

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

    def test_independent_feature_overlap_permitted(self):
        index = next(i for i,e in enumerate(self.events) if e["event"]=="submit")
        self.events[index:index] = [
            dict(event="temporal_record",evaluation=8,feature=9,generation=10,color=11,output=12),
            dict(event="execute_list",queue=8,submission=80,list=81,recording=1),
            dict(event="encode",queue=8,submission=80,list=81,recording=1,encoder=82),
            dict(event="temporal_encode",queue=8,submission=80,encoder=82,evaluation=8,feature=9,generation=10,
                 color=11,output=12,history_sequence=1,reset=1,caller_reset=0),
        ]
        self.events.extend([dict(event="submit",queue=8,submission=80),
            dict(event="complete",queue=8,submission=80,status=4),
            dict(event="temporal_frame_destroy",evaluation=8)])
        for i,e in enumerate(self.events,1): e["event_id"]=i
        report=self.report()
        self.assertTrue(report["pass"],report["errors"])
        # Changing only the feature makes this a real uncommitted same-history
        # overlap and must still fail, rather than removing ordering checks.
        for e in self.events:
            if e.get("evaluation")==8 and e["event"] in ("temporal_record","temporal_encode"):
                e["feature"]=3
        self.assertTrue(any("before the previous commit" in error for error in self.report()["errors"]))

    def test_same_feature_uncommitted_overlap_rejected(self):
        submit=next(e for e in self.events if e["event"]=="submit" and e["submission"]==10)
        consumer=next(e for e in self.events if e["event"]=="temporal_encode" and e["queue"]==2)
        self.events.remove(submit); self.events.insert(self.events.index(consumer)+1,submit)
        for i,e in enumerate(self.events,1):e["event_id"]=i
        self.assertTrue(any("feature 3 encoded another" in error for error in self.report()["errors"]))

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


class Phase3PreparedEvidence(unittest.TestCase):
    """Independent controlled protocols; these are parser tests, not GPU acceptance."""

    def indirect_control(self):
        out, err, ordinal = [], [], 0
        def event(name, **fields):
            nonlocal ordinal
            ordinal += 1
            if name in ("submit", "complete"):
                fields.setdefault("queue", 1)
            err.append("OWNED_GRAPHICS_TRACE " + json.dumps(dict(event=name, event_id=ordinal, **fields)))
        def native(name, **fields):
            err.append("OWNED_GRAPHICS_NATIVE_" + name + " " + " ".join(f"{k}={v}" for k, v in fields.items()))
        submission = 0
        for mode in range(5):
            for maximum in (0, 1, 31, 32, 33, 4097):
                if mode in (2, 4) and maximum != 33:
                    continue
                if maximum and (mode or maximum > 1):
                    event("indirect_compute_resolver", allocator=1, pso=900, max_count=maximum, grid_x=(maximum+31)//32, threads_x=32)
                repetitions = 7 if mode in (1, 3, 4) and maximum == 33 else 1
                for replay in range(repetitions):
                    count = [maximum, 1, maximum, maximum, maximum, 0, maximum + 7][replay]
                    predicate = 0 if replay == 3 else 1
                    active = min(count, maximum) if mode in (1, 3) else maximum
                    if mode in (3, 4) and predicate == 0:
                        active = 0
                    result = active * (active + 1) // 2 if mode in (2, 3) else sum(7 * (i % 3 + 1) for i in range(active) if i % 5 != 4)
                    guards = f"d00d0000,{0xd00d0000+2*maximum:08x},{0xd00d0001+2*maximum:08x}" if mode == 3 else "a17e0011,b28f0022,c3900033"
                    out.append(f"OWNED_D3D12_PREDICATION_INDIRECT_PREPARED_COMPUTE_VALUES mode={mode} max={maximum} replay={replay} count={count} predicate={predicate} result={result} guards={guards}")
                    phase = f"OWNED_D3D12_INDIRECT_PREPARATION_PHASE kind=compute mode={mode} max={maximum} replay={replay} phase="
                    err.append(phase + "submit_begin")
                    submission += 1
                    event("submit", submission=submission)
                    native("DISPATCH", encoder=1, pso=100, mode="threadgroups", grid_x=max(1, (maximum+31)//32), grid_y=1, grid_z=1, threads_x=32, threads_y=1, threads_z=1)
                    if mode in (3, 4):
                        native("DISPATCH", encoder=3, pso=110, mode="threadgroups", grid_x=1, grid_y=1, grid_z=1, threads_x=1, threads_y=1, threads_z=1)
                    if maximum and (mode or maximum > 1):
                        native("DISPATCH", encoder=1, pso=900, mode="threadgroups", grid_x=(maximum+31)//32, grid_y=1, grid_z=1, threads_x=32, threads_y=1, threads_z=1)
                        native("EXECUTE_ICB", stage="compute", encoder=1, buffer=2, location=0, length=maximum)
                    else:
                        for slot in range(maximum):
                            native("DISPATCH_INDIRECT", encoder=1, pso=101, argument_buffer=2, argument_offset=16+32*slot, threads_x=1, threads_y=1, threads_z=1)
                    event("complete", submission=submission, status=4)
                    err.append(phase + "submit_end")
        for kind in range(3):
            for maximum in (0, 1, 2, 4, 63, 64, 65, 4096):
                rgba = "0,0,0,0" if maximum == 0 else "2,5,0,255" if maximum == 1 else "3,7,0,255"
                out.append(f"OWNED_D3D12_PREDICATION_INDIRECT_SIMPLE_GRAPHICS_VALUES kind={kind} max={maximum} rgba={rgba}")
                phase = f"OWNED_D3D12_INDIRECT_PREPARATION_PHASE kind=graphics format={kind} max={maximum} phase="
                err.append(phase + "submit_begin")
                submission += 1
                event("submit", submission=submission)
                if maximum > 64:
                    native("EXECUTE_ICB", stage="render", encoder=1, buffer=2, location=0, length=maximum)
                for slot in range(maximum if maximum <= 64 else 0):
                    fields = dict(encoder=1, argument_buffer=2, argument_offset=16+32*slot, primitive_type=3)
                    if kind:
                        fields.update(index_type=kind-1, index_buffer=3, index_offset=4)
                    native("DRAW_INDEXED_INDIRECT" if kind else "DRAW_INDIRECT", **fields)
                event("complete", submission=submission, status=4)
                err.append(phase + "submit_end")
        for marker in ("PREPARED_COMPUTE_PASS", "SIMPLE_GRAPHICS_PASS", "MSAA_INDEXED_MUTATIONS_PASS counts=1,2 samples=4", "ARENA_BINDINGS_PASS commands=16385 first_last=1 static_sampler=1 vertex_slot=31 cached_statistics_after_growth=1 predicate_skip=1"):
            out.append("OWNED_D3D12_PREDICATION_INDIRECT_" + marker)
        return "\n".join(out), "\n".join(err)

    def test_indirect_numeric_predicate_root_and_tail_corruption(self):
        stdout, stderr = self.indirect_control()
        self.assertEqual(runner.prepared_indirect_failures(stdout, stderr), [])
        for old, new in (("result=0 guards=a17e0011", "result=7 guards=a17e0011"),
                         ("result=1 guards=d00d0000", "result=0 guards=d00d0000"),
                         ("predicate=0 result=0", "predicate=0 result=561"),
                         ("rgba=2,5,0,255", "rgba=1,5,0,255"),
                         ("b28f0022", "b28f0023")):
            with self.subTest(old=old):
                self.assertIn(old, stdout)
                self.assertTrue(runner.prepared_indirect_failures(stdout.replace(old, new, 1), stderr))
        rows = stdout.splitlines()
        for changed in (rows[1:], rows[:1] + rows, [rows[1], rows[0]] + rows[2:]):
            self.assertTrue(runner.prepared_indirect_failures("\n".join(changed), stderr))

    def test_indirect_actual_commands_and_phase_integrity(self):
        stdout, stderr = self.indirect_control()
        for old, new in (("pso=900 mode=threadgroups grid_x=2", "pso=901 mode=threadgroups grid_x=2"),
                         ("threads_x=32", "threads_x=1"),
                         ("argument_offset=48", "argument_offset=16"),
                         ("stage=compute", "stage=render"),
                         ("phase=submit_end", "phase=submit_begin"),
                         ('"status": 4', '"status": 5'),
                         ('"event_id": 2', '"event_id": 1')):
            with self.subTest(old=old):
                self.assertIn(old, stderr)
                self.assertTrue(runner.prepared_indirect_failures(stdout, stderr.replace(old, new, 1)))
        line = next(line for line in stderr.splitlines() if line.startswith("OWNED_GRAPHICS_NATIVE_DISPATCH_INDIRECT"))
        self.assertTrue(runner.prepared_indirect_failures(stdout, stderr.replace(line + "\n", "", 1)))
        self.assertTrue(runner.prepared_indirect_failures(stdout, stderr + "\ntrace limit reached"))
        snapshot=next(line for line in stderr.splitlines() if "pso=110" in line)
        self.assertTrue(runner.prepared_indirect_failures(stdout, stderr.replace(snapshot+"\n", "", 1)))
        self.assertTrue(runner.prepared_indirect_failures(stdout, stderr.replace(snapshot, snapshot.replace("threads_x=1", "threads_x=2"), 1)))

    def test_single_native_compute_only_and_multi_command_parallel_resolver(self):
        out,err=self.indirect_control()
        self.assertEqual(runner.prepared_indirect_failures(out,err),[])
        begin="OWNED_D3D12_INDIRECT_PREPARATION_PHASE kind=compute mode=0 max=32 replay=0 phase=submit_begin"
        end=begin.replace("submit_begin","submit_end")
        part=err.split(begin,1)[1].split(end,1)[0]
        lines=[line for line in part.splitlines() if "pso=900" not in line and not line.startswith("OWNED_GRAPHICS_NATIVE_EXECUTE_ICB")]
        loop=[f"OWNED_GRAPHICS_NATIVE_DISPATCH_INDIRECT encoder=1 pso=101 argument_buffer=2 argument_offset={16+32*i} threads_x=1 threads_y=1 threads_z=1" for i in range(32)]
        self.assertTrue(runner.prepared_indirect_failures(out,err.replace(part,"\n"+"\n".join(lines+loop)+"\n",1)))


    def test_graphics_bypass_threshold_and_actual_icb_range(self):
        out, err = self.indirect_control()
        self.assertEqual(runner.prepared_indirect_failures(out, err), [])
        for kind in range(3):
            for maximum in (64, 65, 4096):
                begin = f"OWNED_D3D12_INDIRECT_PREPARATION_PHASE kind=graphics format={kind} max={maximum} phase=submit_begin"
                end = begin.replace("submit_begin", "submit_end")
                part = err.split(begin, 1)[1].split(end, 1)[0]
                records = [line for line in part.splitlines() if not line.startswith("OWNED_GRAPHICS_NATIVE_")]
                indirect = []
                for slot in range(maximum):
                    line = f"OWNED_GRAPHICS_NATIVE_{'DRAW_INDEXED_INDIRECT' if kind else 'DRAW_INDIRECT'} encoder=1 argument_buffer=2 argument_offset={16+32*slot} primitive_type=3"
                    if kind:
                        line += f" index_type={kind-1} index_buffer=3 index_offset=4"
                    indirect.append(line)
                icb = f"OWNED_GRAPHICS_NATIVE_EXECUTE_ICB stage=render encoder=1 buffer=2 location=0 length={maximum}"
                flipped = [icb] if maximum == 64 else indirect
                with self.subTest(kind=kind, maximum=maximum, mutation="threshold-or-expanded-calls"):
                    self.assertTrue(runner.prepared_indirect_failures(out, err.replace(part, "\n" + "\n".join(records + flipped) + "\n", 1)))
                if maximum > 64:
                    for replacement in ("", icb + "\n" + icb):
                        with self.subTest(kind=kind, maximum=maximum, mutation="missing-or-duplicate-icb"):
                            self.assertTrue(runner.prepared_indirect_failures(out, err.replace(part, part.replace(icb, replacement, 1), 1)))
                    for old, new in (("stage=render", "stage=compute"), ("location=0", "location=1"),
                                     (f"length={maximum}", f"length={maximum-1}")):
                        with self.subTest(kind=kind, maximum=maximum, mutation=new):
                            self.assertTrue(runner.prepared_indirect_failures(out, err.replace(part, part.replace(icb, icb.replace(old, new), 1), 1)))
                    resolver = "OWNED_GRAPHICS_NATIVE_DISPATCH encoder=1 pso=900 mode=threadgroups grid_x=2 grid_y=1 grid_z=1 threads_x=32 threads_y=1 threads_z=1"
                    self.assertTrue(runner.prepared_indirect_failures(out, err.replace(part, part.replace(icb, resolver + "\n" + icb, 1), 1)))


class Phase3UploadEvidence(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        phases = [("cold",20000,0x12340000),("warm",20000,0x23450000)] + [("age-small",1,0x34560000+i) for i in range(51)] + [("after-age",20000,0x45670000),("byte-burst",60000,0x56780000),("after-byte-trim",20000,0x67890000)]
        rows, events, chunks, next_chunk, last_used, created_epoch = [], [], [], 0, {}, {}
        def event(name, **fields):
            if name in ("submit", "complete"):
                fields.setdefault("queue", 1)
            events.append(dict(event=name,event_id=len(events)+1,**fields))
        def create(epoch):
            nonlocal next_chunk
            next_chunk += 1
            chunk = dict(chunk=next_chunk,buffer=next_chunk+100,va=next_chunk*0x1000000,begin=len(chunks)*4194304,capacity=4194304)
            event("root_upload_create",allocator=1,epoch=epoch,reason="initial" if not chunks else "growth",**chunk)
            chunks.append(chunk)
            created_epoch[next_chunk] = epoch
        create(0)
        for _ in range(3):
            event("copy_buffer",bytes=240008)
        event("execute_list",list=2,recording=1,submission=1)
        event("execute_list",list=3,recording=1,submission=1)
        event("submit",submission=1)
        event("signal",value=1)
        event("complete",submission=1,status=4)
        for epoch, (name,count,salt) in enumerate(phases):
            rows.append(f"OWNED_D3D12_UPLOAD_REUSE_BEGIN phase={name} count={count} salt={salt:08x}")
            event("record_begin",list=10,recording=epoch+1,allocator=1)
            for slot in range(count):
                number, local = divmod(slot,13107)
                if number == len(chunks):
                    create(epoch)
                chunk = chunks[number]
                if local == 0 and created_epoch[chunk["chunk"]] < epoch:
                    event("root_upload_reuse",allocator=1,epoch=epoch,**{k:chunk[k] for k in ("chunk","buffer","va","capacity")})
                event("root_upload_allocate",allocator=1,epoch=epoch,chunk=chunk["chunk"],buffer=chunk["buffer"],va=chunk["va"]+local*320,begin=chunk["begin"],offset=local*320,size=272,alignment=64)
                last_used[chunk["chunk"]]=epoch
                event("dispatch",list=10,recording=epoch+1,encoder=epoch+50,x=1,y=1,z=1)
            if epoch==0:
                event("root_upload_reset",allocator=1,epoch=0,accepted=0,reason="in_flight")
            for replay in range(2):
                submission=2+epoch*2+replay
                event("execute_list",list=10,recording=epoch+1,submission=submission)
                event("execute_list",list=3,recording=1,submission=submission)
                event("native_compute_encode",submission=submission,encoder=epoch+50)
                event("submit",submission=submission)
                event("signal",value=submission)
                event("complete",submission=submission,status=4)
            if epoch in (51,54):
                removed=chunks.pop()
                event("root_upload_trim",allocator=1,epoch=epoch+1,last_used_epoch=last_used[removed["chunk"]],reason="age" if epoch==51 else "capacity",**removed)
            event("root_upload_reset",allocator=1,epoch=epoch+1,accepted=1,reason="retired",chunks=len(chunks),retained_bytes=4194304*len(chunks))
            rows.append(f"OWNED_D3D12_UPLOAD_REUSE_PHASE_PASS phase={name} count={count} executions=2")
        rows.append("OWNED_D3D12_UPLOAD_REUSE_PASS growth=20000 burst=60000 age_resets=51 recordings=56 executions=112 guards=60002")
        cls.stdout="\n".join(rows)
        cls.stderr="\n".join("OWNED_GRAPHICS_TRACE "+json.dumps(e) for e in events)

    def test_actual_factory_identity_policy_and_replay(self):
        self.assertEqual(runner.upload_reuse_failures(self.stdout,self.stderr),[])
        mutations=(('"va": 16777216, "begin": 0, "offset": 0','"va": 16777280, "begin": 0, "offset": 0'),
                   ('"alignment": 64','"alignment": 32'),
                   ('"epoch": 0, "accepted": 0','"epoch": 1, "accepted": 0'),
                   ('"last_used_epoch": 1','"last_used_epoch": 2'),
                   ('"accepted": 0','"accepted": 1'))
        for old,new in mutations:
            with self.subTest(old=old):
                self.assertIn(old,self.stderr)
                self.assertTrue(runner.upload_reuse_failures(self.stdout,self.stderr.replace(old,new,1)))
        self.assertTrue(runner.upload_reuse_failures(self.stdout.replace("salt=23450000","salt=23450001",1),self.stderr))
        self.assertTrue(runner.upload_reuse_failures(self.stdout,self.stderr+"\ntrace limit reached"))


class Phase3QueryIntervals(unittest.TestCase):
    def control(self):
        out, err, serial = [], [], 0
        def event(name, **fields):
            nonlocal serial
            serial += 1
            if name in ("submit", "complete"):
                fields.setdefault("queue", 1)
            err.append("OWNED_GRAPHICS_TRACE " + json.dumps(dict(event=name,event_id=serial,**fields)))
        for execution in range(3):
            words=[0x1234000000000000+i for i in range(11)]
            words[1::2]=[12,20,1,0,0] if execution==0 else [10,6,1,0,0]
            out.append(f"OWNED_D3D12_QUERY_INTERVAL_VALUES execution={execution} values="+",".join(map(str,words)))
        out.append("OWNED_D3D12_QUERY_INTERVAL_PASS executions=3 changed_values=1 closed_list_replay=1 crossheap=1 empty_count=0 binary_visible=1 binary_empty=0 guard_words=6 heap_release_before_replay=1")
        for kind, repeats in (("INTERVAL",3),("REUSE",2)):
            if kind=="REUSE":
                for _ in range(4):
                    event("draw",encoder=200,list=2,recording=1)
            for execution in range(repeats):
                encoder=100+min(execution,1) if kind=="INTERVAL" else 200
                if kind=="INTERVAL" and execution<2:
                    err.append(f"OWNED_D3D12_QUERY_INTERVAL_PHASE execution={execution} phase=record_begin")
                    for _ in range(8):
                        event("draw",encoder=encoder,list=1,recording=execution+1)
                    err.append(f"OWNED_D3D12_QUERY_INTERVAL_PHASE execution={execution} phase=record_end")
                err.append(f"OWNED_D3D12_QUERY_{kind}_PHASE execution={execution} phase=submit_begin")
                submission=10+execution if kind=="INTERVAL" else 20+execution
                slots=3 if kind=="INTERVAL" else 2
                event("query_scratch_fill",encoder=encoder,submission=submission,queue=1,slots=slots,bytes=slots*8,fill_count=1)
                event("native_render_begin",encoder=encoder,submission=submission)
                event("native_render_end",encoder=encoder,submission=submission)
                ops=[("query_reset",1,0,None),("query_accumulate",1,0,0)]
                ops += [("query_reset",2,0,None),("query_reset",2,1,None),("query_accumulate",1,0,8),("query_accumulate",2,0,8),("query_accumulate",2,1,8),("query_accumulate",2,0,16),("query_accumulate",2,1,16),("query_reset",1,1,None),("query_reset",2,2,None)] if kind=="INTERVAL" else [("query_reset",1,0,None),("query_accumulate",1,0,8)]
                for name,heap,index,offset in ops:
                    fields=dict(queue=1,submission=submission,heap=heap,index=index)
                    fields.update(encoder=encoder,reset_count=1) if name=="query_reset" else fields.update(source_offset=offset)
                    event(name,**fields)
                event("submit",submission=submission)
                event("complete",submission=submission,status=4)
                err.append(f"OWNED_D3D12_QUERY_{kind}_PHASE execution={execution} phase=submit_end")
                if kind=="REUSE":
                    out.append(f"OWNED_D3D12_QUERY_REUSE_VALUES execution={execution} count=4096")
        out.append("OWNED_D3D12_QUERY_REUSE_PASS executions=2 same_slot=1 heap_release_before_execute=1")
        return "\n".join(out),"\n".join(err)

    def test_exact_interval_empty_reuse_guards_and_native_fanout(self):
        out,err=self.control()
        self.assertEqual(runner.query_interval_failures(out,err),[])
        for old,new in (("values=1311673391471656960,12", "values=1311673391471656960,13"),("count=4096","count=8192")):
            self.assertIn(old,out)
            self.assertTrue(runner.query_interval_failures(out.replace(old,new,1),err))
        for old,new in (('"source_offset": 8','"source_offset": 0'),('"heap": 2','"heap": 1'),('"reset_count": 1','"reset_count": 0'),('"bytes": 24','"bytes": 16'),("phase=submit_end","phase=submit_begin")):
            self.assertIn(old,err)
            self.assertTrue(runner.query_interval_failures(out,err.replace(old,new,1)))
        line=next(line for line in err.splitlines() if '"event": "query_reset"' in line)
        self.assertTrue(runner.query_interval_failures(out,err.replace(line+"\n","",1)))
        self.assertTrue(runner.query_interval_failures(out,err.replace(line,line+"\n"+line,1)))
        self.assertTrue(runner.query_interval_failures(out,err.replace('"event": "query_reset"','"event": "suppressed_reset"',1)))
        guard=str(0x1234000000000002)
        self.assertTrue(runner.query_interval_failures(out.replace(guard,str(int(guard)+1),1),err))


def controlled_msaa_tile_rows(encoder, list_id, producer, *, queue=2, tasks=1, execute=True,
                              released_buffers=11):
    # Normalized synthetic stimuli from final-multisample-capabilities-55-02's
    # 4x4 fallback/query/volatile GPU task traces. These are parser controls,
    # never new GPU evidence and never generated from runner expectations.
    rows = [dict(event="execute_list",queue=queue,submission=producer,list=list_id,recording=1)] if execute else []
    rows.append(dict(event="msaa_tile_begin",queue=queue,encoder=encoder,list=list_id,recording=1,
        submission=producer,source="gpu",capacity=tasks,first_budget=1,quantum_budget=64,
        host_interop="native_scissor",descriptor_capacity=64,descriptor_bytes=1088))
    rows += [dict(event="submit",queue=queue,submission=producer),
             dict(event="complete",queue=queue,submission=producer,status=4),
             dict(event="msaa_tile_ready",queue=queue,encoder=encoder,producer=producer,
                  ready=1,status=0,width=4,height=4,viewports=1,layers=1)]
    for quantum in range(1,tasks+1):
        rows += [dict(event="submit",queue=queue,submission=producer+quantum),
                 dict(event="complete",queue=queue,submission=producer+quantum,status=4),
                 dict(event="msaa_tile_quantum",queue=queue,encoder=encoder,producer=producer+quantum,
                      quantum=quantum,budget=1,count=1,task_done=quantum,status=int(quantum==tasks),
                      ready=int(quantum!=tasks),scissors=1)]
    rows.append(dict(event="msaa_tile_release",queue=queue,encoder=encoder,producer=producer+tasks,
        released_buffers=released_buffers,released_textures=1,released_markers=0,
        retained_buffers=0,retained_textures=0,retained_markers=0))
    return rows


class Phase3NativeMSAA(unittest.TestCase):
    def control(self):
        out, err, serial, encoder, submission = [], [], 0, 100, 0
        prefix="OWNED_D3D12_MULTISAMPLE_CAPABILITIES"
        def event(name, **fields):
            nonlocal serial
            serial+=1
            if name in ("submit", "complete"):
                fields.setdefault("queue", 2 if fields["submission"] >= 10000 else 1)
            err.append("OWNED_GRAPHICS_TRACE "+json.dumps(dict(event=name,event_id=serial,**fields)))
        for resource in ("free", "sample", "volatile", "static"):
            if resource=="sample":
                for count in (2,4,8):
                    for quality in (0,4294967295):
                        for mask in (0xffffffff,0x55555555,0):
                            native_ids=[encoder,encoder+1,encoder+2]
                            native_list,captured_list=encoder+1000,encoder+1001
                            for i,id in enumerate(native_ids):
                                event("native_msaa_record",list=native_list,recording=1,encoder=id,sample_count=count,center=0,index_pull=int(i==2),pso=encoder+2000+int(i==1))
                                event("native_msaa_draw_record",list=native_list,recording=1,encoder=id,indexed=int(i!=0),index_pull=int(i==2),count=3,instances=1)
                            for replay in range(2):
                                for captured in (False,True):
                                    label=f"{'captured_sample_frequency_query' if captured else 'native_sample_frequency'}_count{count}_quality{quality}_mask{mask:08x}_replay{replay}"
                                    submission+=1
                                    err.append(f"{prefix}_SUBMISSION_BEGIN id={submission} label={label}")
                                    producer=10000+submission*16
                                    event("execute_list",queue=2 if captured else 1,
                                          submission=producer if captured else submission,
                                          list=captured_list if captured else native_list,recording=1)
                                    if captured:
                                        event("native_compute_encode",encoder=encoder+4,submission=producer)
                                        for row in controlled_msaa_tile_rows(encoder+3,captured_list,producer,
                                                                            execute=False,released_buffers=10):
                                            event(row.pop("event"),**row)
                                    else:
                                        for id in native_ids:
                                            event("native_render_begin",encoder=id,native_msaa_sample_count=count,submission=submission)
                                            event("native_render_end",encoder=id,submission=submission)
                                    event("submit",submission=submission)
                                    event("complete",submission=submission,status=4)
                                    err.append(f"{prefix}_SUBMISSION_END id={submission} label={label}")
                                samples=4*bin(mask&((1<<count)-1)).count("1")
                                out.append(f"{prefix}_NATIVE_SAMPLE_FREQUENCY_PASS count={count} quality={quality} mask={mask:08x} replay={replay} draws=3 explicit=2 interpolated=1 indexed=1 index_pull=1 pixels=12 channels=4 depth=1 route=native_static")
                                out.append(f"{prefix}_CAPTURED_SAMPLE_FREQUENCY_QUERY_PASS count={count} quality={quality} mask={mask:08x} replay={replay} draws=1 pixels=4 channels=4 depth=1 query_samples={samples} route=captured_query")
                            encoder+=8
                continue
            for count in (2,4,8):
                for quality in (0,4294967295,4294967294):
                    for variant in range(4):
                        mixed=variant==3
                        mask=(0xffffffff,0x55555555,0,0xffffffff)[variant]
                        edge=({2:3,4:15,8:255} if quality==4294967294 else {2:2,4:5,8:58})[count]&mask
                        if resource != "free":
                            static=resource=="static"
                            tag="static_" if static else ""
                            uppercase="STATIC_" if static else ""
                            label=f"native_{tag}resource_transitions_count{count}_quality{quality}_maskffffffff" if mixed else f"native_{tag}resource_inputs_count{count}_quality{quality}_mask{mask:08x}"
                            marker=f"NATIVE_{uppercase}RESOURCE_TRANSITIONS_PASS count={count} quality={quality} draws=4 native_candidates={3 if static else 0} fallback={1 if static else 4} srv=1 cbv=1 msaa_srv=1 owners_released={int(count==8 and quality==4294967294)}" if mixed else f"NATIVE_{uppercase}RESOURCE_INPUTS_PASS count={count} quality={quality} mask={mask:08x} edge={edge} pixels=16 channels=4 depth=1 srv=1 cbv=1 route={'native_static' if static else 'captured_volatile'}"
                        else:
                            label=f"native_fallback_transitions_count{count}_quality{quality}" if mixed else f"native_ordinary_count{count}_quality{quality}_mask{mask:08x}"
                            marker=f"NATIVE_FALLBACK_TRANSITIONS_PASS count={count} quality={quality} draws=4 native_candidates={2 if quality==4294967294 else 3} fallback={2 if quality==4294967294 else 1} indexed=1 index_pull=1" if mixed else f"NATIVE_ORDINARY_PASS count={count} quality={quality} mask={mask:08x} edge={edge} pixels=16 channels=4 depth=1"
                        out.append(prefix+"_"+marker)
                        if resource == "volatile":
                            submission+=1
                            err.append(f"{prefix}_SUBMISSION_BEGIN id={submission} label={label}")
                            for draw in range(4 if mixed else 1):
                                producer=10000+submission*16+draw*3
                                for row in controlled_msaa_tile_rows(encoder+draw,encoder+1000,producer,tasks=2):
                                    event(row.pop("event"),**row)
                            event("submit",submission=submission)
                            event("complete",submission=submission,status=4)
                            err.append(f"{prefix}_SUBMISSION_END id={submission} label={label}")
                            encoder+=4
                            continue
                        native_rows=(0,2,3) if mixed and resource=="static" else (0,1,2) if mixed and quality!=4294967294 else (0,2) if mixed else (0,)
                        native_ids=[encoder+row for row in native_rows]
                        for row,id in zip(native_rows,native_ids):
                            event("native_msaa_record",encoder=id,sample_count=count,center=int(quality==4294967294),index_pull=int(mixed and resource=="free" and row==2),pso=id+1000)
                            event("native_msaa_draw_record",encoder=id,indexed=int(mixed and resource=="free" and row!=1),index_pull=int(mixed and resource=="free" and row==2),count=3 if mixed else 6,instances=1)
                        submission+=1
                        err.append(f"{prefix}_SUBMISSION_BEGIN id={submission} label={label}")
                        for native_index,id in enumerate(native_ids):
                            event("native_render_begin",encoder=id,native_msaa_sample_count=count,submission=submission)
                            event("native_render_end",encoder=id,submission=submission)
                            if mixed and (resource=="free" and (quality==4294967294 or native_index==2) or resource=="static" and native_index==0):
                                producer=10000+submission*16+native_index*3
                                for row in controlled_msaa_tile_rows(
                                        encoder+(3 if resource=="free" and quality!=4294967294 else 1),
                                        encoder+1000,producer):
                                    event(row.pop("event"),**row)
                        event("submit",submission=submission)
                        event("complete",submission=submission,status=4)
                        err.append(f"{prefix}_SUBMISSION_END id={submission} label={label}")
                        encoder+=4
        return "\n".join(out),"\n".join(err)

    def test_actual_native_routes_capture_waits_and_exact_sample_matrix(self):
        out,err=self.control()
        self.assertEqual(runner.native_msaa_failures(out,err),[])
        for old,new in (("edge=2","edge=3"),("owners_released=1","owners_released=0"),("mask=00000000","mask=00000001")):
            self.assertIn(old,out)
            self.assertTrue(runner.native_msaa_failures(out.replace(old,new,1),err))
        for old,new in (('"native_msaa_sample_count": 2','"native_msaa_sample_count": 0'),('"budget": 1','"budget": 0'),("_SUBMISSION_END","_SUBMISSION_BEGIN")):
            self.assertIn(old,err)
            self.assertTrue(runner.native_msaa_failures(out,err.replace(old,new,1)))
        first_ready=next(json.loads(line.split("OWNED_GRAPHICS_TRACE ",1)[1]) for line in err.splitlines() if '"event": "msaa_tile_ready"' in line)
        self.assertTrue(runner.native_msaa_failures(out,err.replace(f'"producer": {first_ready["producer"]}','"producer": 99999',1)))
        for kind in ("msaa_tile_begin","msaa_tile_ready","msaa_tile_quantum","msaa_tile_release"):
            tile_line=next(line for line in err.splitlines() if f'"event": "{kind}"' in line)
            self.assertTrue(runner.native_msaa_failures(out,err.replace(tile_line,tile_line.replace(kind,"ignored_task"),1)))
        self.assertTrue(runner.native_msaa_failures(out,err.replace('"event": "msaa_tile_ready"',
            '"event": "msaa_producer_wait", "wait_count": 1',1)))
        line=next(line for line in err.splitlines() if '"event": "native_msaa_draw_record"' in line)
        self.assertTrue(runner.native_msaa_failures(out,err.replace(line+"\n","",1)))
        self.assertTrue(runner.native_msaa_failures(out,err.replace(line,line+"\n"+line,1)))
        self.assertTrue(runner.native_msaa_failures(out.replace("route=captured_volatile","route=native_static",1),err))
        self.assertTrue(runner.native_msaa_failures(out.replace("native_candidates=0 fallback=4","native_candidates=3 fallback=1",1),err))
        self.assertTrue(runner.native_msaa_failures(out.replace("native_candidates=3 fallback=1","native_candidates=2 fallback=2",1),err))
        self.assertTrue(runner.native_msaa_failures(out.replace("native_candidates=2 fallback=2","native_candidates=3 fallback=1",1),err))
        self.assertTrue(runner.native_msaa_failures(out.replace("NATIVE_STATIC_RESOURCE_INPUTS_PASS","NATIVE_RESOURCE_INPUTS_PASS",1),err))

    def test_sample_frequency_queries_replay_and_route_attribution(self):
        out,err=self.control()
        self.assertEqual(runner.native_msaa_failures(out,err),[])
        native_marker="OWNED_D3D12_MULTISAMPLE_CAPABILITIES_NATIVE_SAMPLE_FREQUENCY_PASS"
        query_marker="OWNED_D3D12_MULTISAMPLE_CAPABILITIES_CAPTURED_SAMPLE_FREQUENCY_QUERY_PASS"
        self.assertEqual(sum(line.startswith(native_marker) for line in out.splitlines()),36)
        self.assertEqual(sum(line.startswith(query_marker) for line in out.splitlines()),36)
        for line in out.splitlines():
            if line.startswith(query_marker):
                fields=dict(field.split("=",1) for field in line.split()[1:])
                samples=int(fields["query_samples"])
                expected={2:(8,4,0),4:(16,8,0),8:(32,16,0)}[int(fields["count"])][("ffffffff","55555555","00000000").index(fields["mask"])]
                self.assertEqual(samples,expected)
                self.assertTrue(runner.native_msaa_failures(out.replace(line,line.replace(f"query_samples={samples}",f"query_samples={samples+4}"),1),err))
        for marker in (native_marker,query_marker):
            line=next(line for line in out.splitlines() if line.startswith(marker) and "replay=1" in line)
            for replacement in ("",line.replace("replay=1","replay=0"),line.replace("route=native_static","route=captured_query") if marker==native_marker else line.replace("route=captured_query","route=native_static")):
                self.assertTrue(runner.native_msaa_failures(out.replace(line,replacement,1),err))
        labels=("native_sample_frequency_count2_quality0_maskffffffff","captured_sample_frequency_query_count2_quality0_maskffffffff")
        def change_group(label,change):
            lines=err.splitlines()
            first=next(i for i,line in enumerate(lines) if "_SUBMISSION_BEGIN" in line and f"label={label}" in line)
            last=next(i for i in range(first+1,len(lines)) if "_SUBMISSION_END" in lines[i])
            events=[json.loads(line.split("OWNED_GRAPHICS_TRACE ",1)[1]) for line in lines[first+1:last]]
            changed=change(events)
            lines[first:last+1]=[] if changed is None else [lines[first]]+["OWNED_GRAPHICS_TRACE "+json.dumps(e) for e in changed]+[lines[last]]
            serial=0
            for i,line in enumerate(lines):
                if "OWNED_GRAPHICS_TRACE " in line:
                    serial+=1
                    e=json.loads(line.split("OWNED_GRAPHICS_TRACE ",1)[1])
                    e["event_id"]=serial
                    lines[i]="OWNED_GRAPHICS_TRACE "+json.dumps(e)
            return "\n".join(lines)
        for label in labels:
            self.assertTrue(runner.native_msaa_failures(out,change_group(label+"_replay1",lambda events: None)))
            self.assertTrue(runner.native_msaa_failures(out,change_group(label+"_replay1",lambda events: [dict(e,recording=2) if e["event"]=="execute_list" else e for e in events])))
        native_label,query_label=(label+"_replay0" for label in labels)
        for kind in ("native_render_begin","native_render_end"):
            self.assertTrue(runner.native_msaa_failures(out,change_group(native_label,lambda events: [dict(e,event="ignored_work") if e["event"]==kind else e for e in events])))
            self.assertTrue(runner.native_msaa_failures(out,change_group(native_label,lambda events: events+[dict(next(e for e in events if e["event"]==kind))])))
        self.assertTrue(runner.native_msaa_failures(out,change_group(native_label,lambda events: events+[dict(event="native_compute_encode",encoder=99999)])))
        self.assertTrue(runner.native_msaa_failures(out,change_group(native_label,lambda events: events+[dict(event="msaa_producer_wait",encoder=99999,queue=2,producer=1,wait_count=1)])))
        self.assertTrue(runner.native_msaa_failures(out,change_group(query_label,lambda events: events+[dict(event="native_render_begin",encoder=99999,native_msaa_sample_count=2)])))
        self.assertTrue(runner.native_msaa_failures(out,change_group(query_label,lambda events: [e for e in events if e["event"]!="msaa_tile_ready"])))
        self.assertTrue(runner.native_msaa_failures(out,change_group(query_label,lambda events: events+[dict(next(e for e in events if e["event"]=="msaa_tile_ready"))])))
        self.assertTrue(runner.native_msaa_failures(out,change_group(query_label,lambda events: [dict(e,producer=e["producer"]+1) if e["event"]=="msaa_tile_ready" else e for e in events])))
        self.assertTrue(runner.native_msaa_failures(out,change_group(labels[1]+"_replay1",lambda events: [dict(e,encoder=99999) if e["event"]=="msaa_tile_begin" else e for e in events])))
        native_first=next(e for line in err.splitlines() if "OWNED_GRAPHICS_TRACE " in line and
            (e:=json.loads(line.split("OWNED_GRAPHICS_TRACE ",1)[1]))["event"]=="native_msaa_record" and "list" in e)
        native_records=[e for line in err.splitlines() if "OWNED_GRAPHICS_TRACE " in line and (e:=json.loads(line.split("OWNED_GRAPHICS_TRACE ",1)[1]))["event"] in ("native_msaa_record","native_msaa_draw_record") and e.get("list")==native_first["list"]]
        self.assertTrue(runner.native_msaa_failures(out,change_group(labels[0]+"_replay1",lambda events: [dict(e,encoder=e["encoder"]+100000) for e in native_records]+[dict(e,encoder=e["encoder"]+100000) if e["event"] in ("native_render_begin","native_render_end") else e for e in events])))
        for kind,field,value in (("native_msaa_draw_record","indexed",1),("native_msaa_draw_record","index_pull",1),("native_msaa_draw_record","count",6),("native_msaa_draw_record","instances",2),("native_msaa_draw_record","event","ignored_work"),("native_msaa_record","sample_count",4),("native_msaa_record","index_pull",1)):
            line=next(line for line in err.splitlines() if "OWNED_GRAPHICS_TRACE " in line and (e:=json.loads(line.split("OWNED_GRAPHICS_TRACE ",1)[1]))["event"]==kind and e.get("list")==native_first["list"])
            changed=json.loads(line.split("OWNED_GRAPHICS_TRACE ",1)[1])
            changed[field]=value
            self.assertTrue(runner.native_msaa_failures(out,err.replace(line,"OWNED_GRAPHICS_TRACE "+json.dumps(changed),1)))


class Phase3NativeBudget(unittest.TestCase):
    def test_mixed_180_budget_and_legacy_interleaving_preserves_every_record(self):
        native_out,native_err=Phase3NativeMSAA().control()
        budget_out,budget_err=self.control()
        offset=sum("OWNED_GRAPHICS_TRACE " in line for line in native_err.splitlines())
        shifted=[]
        last_submission=max(int(m[1]) for line in native_err.splitlines() if (m:=re.search(r"_SUBMISSION_BEGIN id=([0-9]+)",line)))
        for submission,label in enumerate(("native_resource_upload","native_static_resource_upload"),last_submission+1):
            native_err+=f"\nOWNED_D3D12_MULTISAMPLE_CAPABILITIES_SUBMISSION_BEGIN id={submission} label={label}\nOWNED_D3D12_MULTISAMPLE_CAPABILITIES_SUBMISSION_END id={submission} label={label}"
        for line in budget_err.splitlines():
            if "OWNED_GRAPHICS_TRACE " in line:
                event=json.loads(line.split("OWNED_GRAPHICS_TRACE ",1)[1])
                event["event_id"]+=offset
                for field in ("encoder","list","pso"):
                    if field in event:
                        event[field]+=10000
                shifted.append("OWNED_GRAPHICS_TRACE "+json.dumps(event))
            else:
                shifted.append(line)
        out=native_out+"\n"+budget_out
        err=native_err+"\n"+"\n".join(shifted)
        prefix="OWNED_D3D12_MULTISAMPLE_CAPABILITIES"
        begin=f"{prefix}_SUBMISSION_BEGIN id={submission+1} label=existing"
        end=f"{prefix}_SUBMISSION_END id={submission+1} label=existing"
        event=dict(event="encode",event_id=offset+sum("OWNED_GRAPHICS_TRACE " in line for line in shifted)+1,queue=6,submission=7,list=10010,recording=2,encoder=10022,type=2)
        trace="info:  OWNED_GRAPHICS_TRACE "+json.dumps(event)
        separate=err+"\n"+begin+"\n"+trace+"\n"+end
        merged=err+"\n"+begin+trace+"\n"+end
        for helper in (runner.native_msaa_failures,runner.native_msaa_budget_failures):
            self.assertEqual(helper(out,separate),[])
            self.assertEqual(helper(out,merged),[])
            for changed in (merged.replace(begin+trace,trace,1),merged.replace(begin+trace,begin+"\n"+begin+trace,1),merged.replace(end,end.replace(f"id={submission+1}",f"id={submission+2}"),1),merged.replace(begin+trace,begin+trace[:-1],1),merged.replace(begin+trace,begin+trace+"garbage",1),merged+"\n"+trace,merged.replace(begin+trace,begin+trace.replace(f'"event_id": {event["event_id"]}','"event_id": 99999'),1),merged+f"\n{prefix}_SUBMISSION_BEGIN id={submission+2} label=existing truncated"):
                self.assertTrue(helper(out,changed),helper.__name__)
        lines=separate.splitlines()
        index=next(i for i,line in enumerate(lines) if "_SUBMISSION_BEGIN" in line and "label=native_ordinary_" in line)
        forward="\n".join(lines[:index]+[lines[index]+"info:  "+lines[index+1]]+lines[index+2:])
        index=next(i for i,line in enumerate(lines) if "_SUBMISSION_END" in line and "label=native_ordinary_" in line)
        reverse="\n".join(lines[:index-1]+["info:  "+lines[index-1]+lines[index]]+lines[index+1:])
        for changed in (forward,reverse):
            self.assertEqual(runner.native_msaa_failures(out,changed),[])
            self.assertEqual(runner.native_msaa_budget_failures(out,changed),[])
        self.assertTrue(runner.native_msaa_failures(out,merged.replace("label=native_ordinary_count2","label=native_unknown_count2",1)))
        self.assertTrue(runner.native_msaa_failures(out,merged.replace("label=captured_sample_frequency_query_count2","label=captured_sample_frequency_unknown_count2",1)))
        self.assertTrue(runner.native_msaa_failures(out,merged.replace("label=native_binding_budget_first","label=native_binding_budget_unknown",1)))

    def control(self):
        prefix="OWNED_D3D12_MULTISAMPLE_CAPABILITIES"
        err=[]
        serial=0
        def event(name, **fields):
            nonlocal serial
            serial+=1
            err.append("OWNED_GRAPHICS_TRACE "+json.dumps(dict(event=name,event_id=serial,**fields)))
        err.append(prefix+"_NATIVE_BINDING_BUDGET_RECORDING_BEGIN draws=66 committed_cbvs=65 repeated_bindings=1 om_binds=1")
        event("native_msaa_record",list=10,recording=1,encoder=20,sample_count=2,center=0,index_pull=0,pso=100)
        for draw in range(66):
            if draw==64:
                event("native_msaa_budget_rollover",list=10,recording=1,encoder=20,buffer_snapshots=64,table_snapshots=0,limit=64,cause="binding_budget")
                event("native_msaa_record",list=10,recording=1,encoder=21,sample_count=2,center=0,index_pull=0,pso=100)
            event("native_msaa_draw_record",list=10,recording=1,encoder=20 if draw<64 else 21,indexed=0,index_pull=0,count=3,instances=1)
        err.append(prefix+"_NATIVE_BINDING_BUDGET_RECORDING_CLOSED draws=66 committed_cbvs=65 repeated_bindings=1 owners_released=1")
        labels=("first","color_observe","depth_observe","replay","color_observe","depth_observe")
        for index,label in enumerate(labels,1):
            err.append(f"{prefix}_SUBMISSION_BEGIN id={index} label=native_binding_budget_{label}")
            if label in ("first","replay"):
                event("execute_list",queue=6,submission=index,list=10,recording=1)
                for encoder in (20,21):
                    event("native_render_begin",encoder=encoder,submission=index,native_msaa_sample_count=2)
                    event("native_render_end",encoder=encoder,submission=index)
            event("submit",queue=6,submission=index)
            event("complete",queue=6,submission=index,status=4)
            err.append(f"{prefix}_SUBMISSION_END id={index} label=native_binding_budget_{label}")
        out="\n".join(f"{prefix}_NATIVE_BINDING_BUDGET_PASS replay={i} draws=66 committed_cbvs=65 repeated_bindings=1 om_binds=1 count=2 quality=0 pixels=272 channels=4 depth=1 guard_pixels=8 owners_released=1 route=native_static" for i in range(2))
        return out,"\n".join(err)

    def test_capacity_rollover_and_retained_replay_are_actual_work(self):
        out,err=self.control()
        self.assertEqual(runner.native_msaa_budget_failures(out,err),[])
        for old,new in (('"buffer_snapshots": 64','"buffer_snapshots": 63'),('"table_snapshots": 0','"table_snapshots": 1'),('"limit": 64','"limit": 65'),('"cause": "binding_budget"','"cause": "om_rebind"'),('"native_msaa_sample_count": 2','"native_msaa_sample_count": 0'),('"recording": 1','"recording": 2'),('"event": "native_render_end"','"event": "msaa_producer_wait"'),("label=native_binding_budget_replay","label=native_binding_budget_first")):
            self.assertTrue(runner.native_msaa_budget_failures(out,err.replace(old,new,1)),old)
        for old,new in (("replay=1","replay=0"),("committed_cbvs=65","committed_cbvs=64"),("guard_pixels=8","guard_pixels=0"),("owners_released=1","owners_released=0"),("pixels=272","pixels=264")):
            self.assertTrue(runner.native_msaa_budget_failures(out.replace(old,new,1),err),old)
        for kind in ("native_msaa_budget_rollover","native_msaa_draw_record","native_render_begin"):
            line=next(line for line in err.splitlines() if f'"event": "{kind}"' in line)
            self.assertTrue(runner.native_msaa_budget_failures(out,err.replace(line+"\n","",1)))
            self.assertTrue(runner.native_msaa_budget_failures(out,err.replace(line,line+"\n"+line,1)))
            self.assertTrue(runner.native_msaa_budget_failures(out,err.replace(line,line.replace(f'"event": "{kind}"','"event": "ignored_work"'),1)))
        repeat=json.loads(next(line.split("OWNED_GRAPHICS_TRACE ",1)[1] for line in err.splitlines() if '"event": "native_msaa_budget_rollover"' in line))
        repeat["event_id"]=sum("OWNED_GRAPHICS_TRACE " in line for line in err.splitlines())+1
        self.assertTrue(runner.native_msaa_budget_failures(out,err+"\nOWNED_GRAPHICS_TRACE "+json.dumps(repeat)))


class Phase3ConcurrentTrace(unittest.TestCase):
    def test_multiqueue_identity_and_independent_logger_interleave(self):
        events=[dict(event="submit",event_id=1,queue=6,submission=12),dict(event="submit",event_id=2,queue=207,submission=12),dict(event="native_blit_begin",event_id=3,encoder=148,submission=13),dict(event="native_blit_end",event_id=5,encoder=148,submission=13),dict(event="complete",event_id=4,queue=6,submission=12,status=4),dict(event="complete",event_id=6,queue=207,submission=12,status=4)]
        text="\n".join("OWNED_GRAPHICS_TRACE "+json.dumps(e) for e in events)
        parsed,failures=runner.phase3_trace(text)
        self.assertEqual(failures,[])
        self.assertEqual([e["event_id"] for e in parsed],[1,2,3,5,4,6])
        for altered in (events[:4]+events[5:],events+[events[0]],events[:4]+[dict(events[4],queue=207)]+events[5:],events[:2]+[dict(events[2],event_id=5),dict(events[3],event_id=3)]+events[4:], [dict(events[0],event_id=4)]+events[1:4]+[dict(events[4],event_id=1)]+events[5:]):
            with self.subTest(altered=altered):
                self.assertTrue(runner.phase3_trace("\n".join("OWNED_GRAPHICS_TRACE "+json.dumps(e) for e in altered))[1])
        fifo=[dict(event="submit",event_id=1,queue=6,submission=1),dict(event="submit",event_id=2,queue=6,submission=2),dict(event="complete",event_id=4,queue=6,submission=2,status=4),dict(event="complete",event_id=3,queue=6,submission=1,status=4)]
        self.assertTrue(runner.phase3_trace("\n".join("OWNED_GRAPHICS_TRACE "+json.dumps(e) for e in fifo))[1])
        self.assertTrue(runner.phase3_trace(text,limit=6)[1])
        self.assertTrue(runner.phase3_trace(text+"\ntrace limit reached",limit=100000)[1])

    def test_known_native_binding_siblings_do_not_replace_commands(self):
        out,err=Phase3PreparedEvidence().indirect_control()
        metadata="OWNED_GRAPHICS_NATIVE_BINDING table_new=1 table_set=2 table_bind=3 residency_new=1 residency_add=2 residency_remove=0 residency_commit=1"
        err="\n".join((metadata+"\n"+line) if line.startswith("OWNED_D3D12_INDIRECT_PREPARATION_PHASE") else line for line in err.splitlines())
        self.assertEqual(runner.prepared_indirect_failures(out,err),[])
        command=next(line for line in err.splitlines() if line.startswith("OWNED_GRAPHICS_NATIVE_DISPATCH_INDIRECT"))
        self.assertTrue(runner.prepared_indirect_failures(out,err.replace(command+"\n","",1)))
        self.assertTrue(runner.prepared_indirect_failures(out,err.replace(command,command+"\n"+command,1)))
        self.assertTrue(runner.prepared_indirect_failures(out,err.replace(command,command.replace("argument_buffer=2","argument_buffer=-2"),1)))
        self.assertTrue(runner.prepared_indirect_failures(out,err.replace(metadata,metadata.replace("table_set=2","table_set=no"),2)))


class NativeAttachmentMetadata(unittest.TestCase):
    def test_attachment_metadata_does_not_replace_indirect_work(self):
        out, original = Phase3PreparedEvidence().indirect_control()
        metadata = "OWNED_GRAPHICS_NATIVE_RENDER_ATTACHMENT encoder=12 aspect=4 slot=0 texture=25 load=2 store=1"
        command = next(line for line in original.splitlines() if line.startswith("OWNED_GRAPHICS_NATIVE_DRAW_INDIRECT"))
        err = original.replace(command, metadata + "\n" + command, 1)
        self.assertEqual(runner.prepared_indirect_failures(out, err), [])
        self.assertTrue(runner.prepared_indirect_failures(out, err.replace(command + "\n", "", 1)))
        self.assertTrue(runner.prepared_indirect_failures(out, err.replace(command, command + "\n" + command, 1)))
        for old, new in (("encoder=12", "encoder=0"), ("texture=25", "texture=0"),
                         ("aspect=4", "aspect=8"), ("slot=0", "slot=8"),
                         ("load=2", "load=-2"), ("store=1", ""), ("slot=0", "slot=0 slot=0")):
            with self.subTest(old=old, new=new):
                self.assertTrue(runner.prepared_indirect_failures(out, err.replace(metadata, metadata.replace(old, new))))


class LowLatencyEvidence(unittest.TestCase):
    def stderr(self, mode="forced"):
        events = [dict(event="submit", queue=1, submission=1),
                  dict(event="complete", queue=1, submission=1, status=4)]
        if mode != "off":
            for frame in range(2):
                events.append(dict(event="low_latency_admit", scheduler=100, frame=frame + 1,
                    source=2 if mode == "antilag2" else 0, now_ns=1000 + frame * 100,
                    target_ns=0, pending_frames=1, capacity=0))
        return ('Loaded L"C:\\windows\\system32\\amdxc64.dll" at 0001: builtin\n'
                + "".join("OWNED_GRAPHICS_TRACE " + json.dumps({**event, "event_id": index + 1}) + "\n"
                          for index, event in enumerate(events)))

    def trace(self, mode="forced", supported=True):
        prefix = "OWNED_D3D12_LOW_LATENCY"
        lines = [f"{prefix}_BACKEND id=owned-graphics-d3d12",
                 f"{prefix}_CLOCK cpu_frequency=1000000 gpu_supported={int(supported)} "
                 f"gpu_frequency={1000000000 if supported else 0} "
                 f"gpu_status=0x{'00000000' if supported else '80004001'}"]
        for frame in range(2):
            cpu, gpu = 100 + frame * 100, 1000 + frame * 1000
            lines.append(f"{prefix}_FRAME frame={frame} input_begin={cpu} input_end={cpu + 1} "
                         f"submit_begin={cpu + 2} submit_end={cpu + 3} "
                         f"present_begin={cpu + 4} present_end={cpu + 5} "
                         f"gpu_begin={gpu if supported else 0} gpu_end={gpu + 500 if supported else 0} "
                         "pixels=230400 readback=exact")
        if mode == "antilag2":
            lines.append(f"{prefix}_AMD contracts=init,options,identity,lifetime,version,errors,input,fg,end-render "
                         "backend=d3dalloy-owned-antilag2")
        lines.append(f"{prefix}_PASS mode={mode} frames=2 verified=2 width=640 height=360 "
                     f"draws=64 slots=8 amd_enable={int(mode == 'antilag2')} reflex_enable=0")
        return "\n".join(lines) + "\n"

    def test_measurement_contract_rejects_incomplete_or_fabricated_timestamps(self):
        trace = self.trace()
        corruptions = (
            trace.replace("frame=1 ", "frame=0 ", 1),
            trace.replace("readback=exact", "readback=failed", 1),
            trace.replace("gpu_begin=1000", "gpu_begin=0", 1),
            trace.replace("gpu_end=1500", "gpu_end=999", 1),
            trace.replace("submit_end=103", "submit_end=99", 1),
            trace.replace("verified=2", "verified=1"),
            trace.replace("amd_enable=0", "amd_enable=1"),
            trace.replace("cpu_frequency=1000000", "cpu_frequency=0"),
            trace + next(line for line in trace.splitlines() if "_FRAME " in line) + "\n",
        )
        for performance in (False, True):
            stderr = "" if performance else self.stderr()
            with self.subTest(performance=performance):
                self.assertEqual(runner.low_latency_failures(trace, stderr, "forced", 2,
                                                            performance=performance), [])
            for broken in corruptions:
                with self.subTest(performance=performance, broken=broken):
                    self.assertTrue(runner.low_latency_failures(broken, stderr, "forced", 2,
                                                               performance=performance))

    def test_unavailable_gpu_timing_is_explicit_not_synthetic(self):
        trace = self.trace(supported=False)
        self.assertEqual(runner.low_latency_failures(trace, self.stderr(), "forced", 2), [])
        self.assertTrue(runner.low_latency_failures(trace.replace("gpu_begin=0", "gpu_begin=1", 1), self.stderr(), "forced", 2))
        self.assertTrue(runner.low_latency_failures(trace.replace("80004001", "80004005"), self.stderr(), "forced", 2))

    def test_cooperative_contract_requires_builtin_AMD_evidence(self):
        trace = self.trace(mode="antilag2")
        for performance in (False, True):
            builtin = (self.stderr(mode="antilag2").splitlines()[0] + "\n"
                       if performance else self.stderr(mode="antilag2"))
            with self.subTest(performance=performance):
                self.assertEqual(runner.low_latency_failures(trace, builtin, "antilag2", 2,
                                                            performance=performance), [])
                self.assertTrue(runner.low_latency_failures(trace, builtin.replace("builtin", "native"), "antilag2", 2,
                                                           performance=performance))
                self.assertTrue(runner.low_latency_failures(trace.replace("_AMD contracts=", "_MISSING contracts="),
                                                           builtin, "antilag2", 2, performance=performance))
                self.assertTrue(runner.low_latency_failures(trace, builtin, "forced", 2,
                                                           performance=performance))

    def test_trace_free_measurement_requires_explicit_performance_policy(self):
        for mode in ("off", "forced", "antilag2"):
            trace = self.trace(mode=mode)
            stderr = self.stderr(mode=mode)
            modules_only = stderr.splitlines()[0] + "\n"
            with self.subTest(mode=mode):
                self.assertTrue(runner.low_latency_failures(trace, modules_only, mode, 2))
                self.assertEqual(runner.low_latency_failures(trace, modules_only, mode, 2,
                                                            performance=True), [])
                self.assertTrue(runner.low_latency_failures(trace, stderr, mode, 2, performance=True))

    def test_performance_policy_rejects_other_suites_before_creating_evidence(self):
        for suite in ("smoke", "nvapi-reflex"):
            with self.subTest(suite=suite), self.assertRaises(ValueError):
                runner.run(argparse.Namespace(suite=suite, low_latency_performance=True))

    def test_scheduler_ownership_and_real_completion_are_required(self):
        trace, stderr = self.trace(), self.stderr()
        for broken in (stderr.replace('"source": 0', '"source": 2', 1),
                       stderr.replace('"frame": 2', '"frame": 1'),
                       stderr.replace('"capacity": 0', '"capacity": 2', 1),
                       stderr.replace('"event": "low_latency_admit"', '"event": "low_latency_mode"', 1),
                       stderr.replace('"status": 4', '"status": 5')):
            with self.subTest(broken=broken):
                self.assertTrue(runner.low_latency_failures(trace, broken, "forced", 2))
        self.assertTrue(runner.low_latency_failures(self.trace(mode="off"), stderr, "off", 2))

    def test_runner_strips_inherited_config_and_composes_only_explicit_options(self):
        for mode, force, compose, performance in (
                ("off", False, False, False), ("forced", True, False, False),
                ("off", False, True, False), ("forced", True, True, False),
                ("antilag2", False, True, False), ("antilag2", True, True, False),
                ("off", False, False, True), ("forced", True, True, True),
                ("antilag2", False, False, True)):
            with self.subTest(mode=mode, force=force, compose=compose, performance=performance), tempfile.TemporaryDirectory(dir=SCRATCH, prefix="latency-") as directory:
                base = Path(directory)
                runtime = base / "runtime"
                native = runtime / "lib/wine/x86_64-unix/winemetal.so"
                native.parent.mkdir(parents=True)
                native.write_bytes(b"controlled native module")
                manifest = runtime / "owned-graphics-runtime.json"
                manifest.write_text('{"schema": 1}\n')
                fixture = base / "latency.exe"
                fixture.write_bytes(controlled_pe_fixture())
                args = argparse.Namespace(runtime=runtime, lock=manifest, fixture=fixture, dxil_dir=base,
                    out=base / "result", suite="low-latency", mode="present", validation=False,
                    api_validation=False, timeout=1, low_latency_mode=mode, low_latency_frames=2,
                    low_latency_performance=performance, force_low_latency=force,
                    async_pipeline_compile=compose, auto_breadcrumbs=compose)
                observed_config = []

                def fake_run(command, **kwargs):
                    if command[0] == str(runtime / "bin/wine"):
                        observed_config.append(kwargs["env"].get("D3DALLOY_CONFIG"))
                        self.assertNotIn("D3DALLOY_CONFIG_FILE", kwargs["env"])
                        kwargs["stdout"].write(self.trace(mode=mode))
                        fixture_stderr = self.stderr(mode=mode)
                        if performance:
                            fixture_stderr = fixture_stderr.splitlines()[0] + "\n"
                        kwargs["stderr"].write(f"dyld[1]: {native}\n" + fixture_stderr)
                    return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

                with patch.object(runner.subprocess, "run", side_effect=fake_run), \
                     patch.dict(runner.os.environ, {"D3DALLOY_CONFIG": "d3d12.lowLatency=True;foreign=1",
                                                   "D3DALLOY_CONFIG_FILE": "/foreign/settings"}), \
                     contextlib.redirect_stdout(io.StringIO()):
                    result = runner.run(args)
                expected = "d3d12.asyncPipelineCompile=True;d3d12.enableAutoBreadcrumbs=True" if compose else ""
                if force:
                    expected = ";".join(filter(None, (expected, "d3d12.lowLatency=True")))
                self.assertEqual(observed_config, [expected or None])
                self.assertEqual(result, 0)
                report = json.loads((args.out / "result.json").read_text())
                self.assertEqual(report["explicit_configuration"], expected or None)
                self.assertEqual(report["force_low_latency"], force)
                self.assertEqual(report["low_latency_performance"], performance)
                self.assertEqual(report["low_latency_trace_assertions_exercised"], not performance)
                self.assertEqual(report["owned_graphics_trace_enabled"], not performance)
                self.assertEqual(report["logger_level"], "warn" if performance else "info")
                self.assertTrue(report["pass"])


class Phase3PipelineMSAA(unittest.TestCase):
    def test_consumed_coverage_and_native_control_require_numeric_completion(self):
        prefix="OWNED_D3D12_PIPELINE_LIBRARY"
        out="\n".join(f"{prefix}_GPU case={case} texels=16 value=ff332211" for case in ("captured_layout_first","captured_layout_reuse","ordinary_native2x"))
        events=[dict(event="native_msaa_draw_record",encoder=1,count=3,instances=1),dict(event="native_render_begin",encoder=1,native_msaa_sample_count=2,submission=4),dict(event="native_render_end",encoder=1),dict(event="complete",submission=4,status=4)]
        err=f"{prefix}_NATIVE_MSAA_BEGIN count=2\n"+"\n".join("OWNED_GRAPHICS_TRACE "+json.dumps(e) for e in events)+f"\n{prefix}_NATIVE_MSAA_END count=2 resolved=ff332211"
        for ordinal,phase in enumerate(("captured_layout_cold","captured_layout_hit")):
            capture=[dict(row,event_id=index+1) for index,row in enumerate(
                controlled_msaa_tile_rows(3,5,10+ordinal*2,queue=6))]
            err+=f"\n{prefix}_ARTIFACT_PHASE begin={phase}\n"+"\n".join("OWNED_GRAPHICS_TRACE "+json.dumps(e) for e in capture)+f"\n{prefix}_ARTIFACT_PHASE end={phase} air={5 if phase.endswith('cold') else 0} libraries_max={5 if phase.endswith('cold') else 0}"
        self.assertEqual(runner.pipeline_native_msaa_failures(out,err),[])
        self.assertTrue(runner.pipeline_native_msaa_failures(out.replace("value=ff332211","value=ff332212",1),err))
        self.assertTrue(runner.pipeline_native_msaa_failures(out,err.replace('"status": 4','"status": 5')))
        self.assertTrue(runner.pipeline_native_msaa_failures(out,err.replace('"native_msaa_sample_count": 2','"native_msaa_sample_count": 0')))
        self.assertTrue(runner.pipeline_native_msaa_failures(out,err.replace('"event": "native_render_end"','"event": "msaa_producer_wait"')))
        self.assertTrue(runner.pipeline_native_msaa_failures(out,err.replace('"budget": 1','"budget": 0',1)))
        self.assertTrue(runner.pipeline_native_msaa_failures(out,err.replace('"producer": 10','"producer": 11',1)))
        self.assertTrue(runner.pipeline_native_msaa_failures(out,err.replace('"event": "msaa_tile_ready"','"event": "native_render_begin"',1)))
        self.assertTrue(runner.pipeline_native_msaa_failures(out,err.replace('"event": "msaa_tile_ready"',
            '"event": "msaa_producer_wait", "wait_count": 1',1)))
        self.assertTrue(runner.pipeline_native_msaa_failures(out,err.replace('"retained_buffers": 0','"retained_buffers": 1',1)))
        self.assertTrue(runner.pipeline_native_msaa_failures(out,err.replace('"event": "submit"','"event": "fake_submit"',1)))
        malformed=next(line for line in err.splitlines() if '"event": "native_msaa_draw_record"' in line)
        self.assertTrue(runner.pipeline_native_msaa_failures(out,err.replace(malformed,"OWNED_GRAPHICS_TRACE []",1)))


if __name__ == "__main__":
    SCRATCH.mkdir(parents=True, exist_ok=True)
    unittest.main(verbosity=2)
