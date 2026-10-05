#!/usr/bin/env python3
"""Corroborate public consumer GPU pixels with real native drawables/timestamps."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def records(text, marker):
    return [dict(re.findall(r"(\w+)=([^\s]+)", line))
            for line in text.splitlines() if line.startswith(marker + " ")]


def statistics_observations(rows, count_key, previous=(0, 0, 0, 0)):
    observations = []
    for row in rows:
        submitted, frequency = int(row["submitted_present"]), int(row["frequency"])
        values = tuple(int(row[key]) for key in (count_key, "sync_qpc", "present_refresh", "sync_refresh"))
        require(submitted >= 0 and frequency > 0 and int(row["qpc_end"]) > 0, "invalid statistics submission/clock")
        outcome, result = row["statistics"], int(row["result"], 0)
        if outcome == "actual":
            count, qpc, refresh, sync = values
            require(result == 0 and 0 < count <= submitted and qpc > 0 and 0 < refresh <= sync,
                    "invalid measured statistics count/refresh/clock/result")
            require(all(a <= b for a, b in zip(previous, values)), "measured statistics regress")
            require(qpc <= int(row["qpc_end"]), "measured statistics clock is in the future")
            previous = values
        else:
            require(outcome == "disjoint" and result == 0x887A000B and values == (0, 0, 0, 0),
                    "disjoint statistics must preserve HRESULT and zero unmeasured fields")
        observations.append({"statistics": outcome, "result": result, "submitted_present": submitted,
                             "displayed_count": values[0], "sync_qpc": values[1]})
    return observations, previous


def rgba(data, width, height, fmt):
    require(width > 0 and height > 0, "invalid native drawable dimensions")
    if fmt in (70, 71, 80, 81):
        require(len(data) == width * height * 4, "native 8-bit image size mismatch")
        if fmt in (80, 81):
            result = bytearray(data)
            result[0::4], result[2::4] = data[2::4], data[0::4]
            return result
        return data
    require(fmt == 115 and len(data) == width * height * 8, "unsupported native pixel format")
    result = bytearray()
    for i, (value,) in enumerate(struct.iter_unpack("<e", data)):
        require(math.isfinite(value), "nonfinite native pixel")
        if i % 4 != 3:
            value = 12.92 * value if value <= .0031308 else 1.055 * max(value, 0) ** (1 / 2.4) - .055
        result.append(round(min(max(value, 0), 1) * 255))
    return result


def check_ngx(args):
    stdout, stderr = args.stdout.read_text(), args.stderr.read_text()
    prefix = "OWNED_D3D12_NGX_PRESENT"
    summary = records(stdout, prefix + "_PASS")
    require(len(summary) == 1 and all(summary[0].get(key) == value for key, value in
            {"frames": "6", "drawables": "9", "generated": "3", "suppressed": "3", "ngx_feature": "11",
             "hudless": "1", "release_before_execute": "1", "replay": "exact", "caller_owned_present": "1"}.items()),
            "NGX public consumer incomplete")
    require(prefix + "_OBSERVER_READY public_selectors=1" in stderr, "NGX native observer never loaded")
    require(prefix + "_OBSERVER_FAIL" not in stderr, "NGX native observer GPU failure")
    expected = [(frame, generated) for frame in range(1, 7)
                for generated in ((1, 0) if frame in (2, 3, 4) else (0,))]
    calls = records(stdout, prefix + "_FRAME")
    require([(int(r["frame"]), int(r["generated"])) for r in calls] == expected,
            "NGX generated-before-real public sequence incomplete/reordered")
    suppressed = records(stdout, "OWNED_D3D12_NGX_SUPPRESSED")
    require([int(r["frame"]) for r in suppressed] == [1, 5, 6] and all(
            r.get("actual") == "0" and r.get("flag") == "1" and r.get("output_untouched") == "1" for r in suppressed),
            "NGX actual0 GPU flag/suppression sequence incomplete")
    bootstrap = records(stdout, prefix + "_BOOTSTRAP")
    ready = records(stdout, prefix + "_BOOTSTRAP_READY")
    require(1 <= len(bootstrap) <= 64 and [int(r["attempt"]) for r in bootstrap] == list(range(1, len(bootstrap) + 1)),
            "NGX separately numbered real bootstrap sequence missing/reordered")
    require(len(ready) == 1 and int(ready[0]["attempt"]) == len(bootstrap),
            "NGX bootstrap must end exactly at its observed visibility-ready attempt")
    drawables, events = records(stderr, prefix + "_DRAWABLE"), records(stderr, prefix + "_PRESENTED")
    require(len(drawables) == len(events) == len(bootstrap) + len(expected),
            "NGX native capture/presented count must cover validated bootstrap plus exactly nine actual outputs")
    presented = {int(r["sequence"]): r for r in events}
    require(len(presented) == len(events), "duplicate NGX native presented event")
    fixture = args.stdout.parent / "fixture"
    sentinel = bytes((19,)) * (128 * 128 * 4)
    for row in suppressed:
        require((fixture / f"fg-ngx-{row['frame']}-1.bin").read_bytes() == sentinel,
                "NGX suppressed actual0 output changed")
    boot_images = []
    previous_sequence = previous_count = 0
    last_boot_time = 0
    for record, call in zip(drawables[:len(bootstrap)], bootstrap, strict=True):
        attempt, count = int(call["attempt"]), int(call["submitted_present"])
        require(count == previous_count + 1 and call["rgb"] == f"{16 + attempt},80,176"
                and call["source"] == "caller_real_gpu_copy_to_dxgi", "NGX bootstrap is not the independent real-color protocol")
        source = (fixture / f"fg-ngx-bootstrap-{attempt}.bin").read_bytes()
        require(source == bytes((16 + attempt, 80, 176, 255)) * (128 * 128),
                "NGX bootstrap real GPU readback differs from independent solid-color pixels")
        sequence = int(record["sequence"])
        require(sequence > previous_sequence, "NGX bootstrap native requests reordered")
        width, height, fmt = (int(record[k]) for k in ("width", "height", "format"))
        path = args.captures / Path(record["path"]).name
        require(not path.is_symlink() and path.is_file(), "NGX bootstrap native capture is not a regular file")
        raw = path.read_bytes(); actual = rgba(raw, width, height, fmt)
        for nx, ny in ((.03, .03), (.25, .25), (.5, .5), (.75, .75), (.97, .97)):
            pixel = (min(height - 1, int(ny * height)) * width + min(width - 1, int(nx * width))) * 4
            require(all(abs(actual[pixel + c] - color) <= 3 for c, color in enumerate((16 + attempt, 80, 176))),
                    f"NGX bootstrap native drawable pixels mismatch attempt={attempt}")
        event = presented.get(sequence)
        require(event and event["drawable"] == record["drawable"], "NGX bootstrap captured drawable lacks native presented event")
        time = float(event["time"])
        require(math.isfinite(time) and time >= 0, "NGX bootstrap has invalid native presentation timestamp")
        if time:
            require(time >= last_boot_time and (not last_boot_time or time - last_boot_time < 15),
                    "NGX bootstrap actual display timestamps reordered/unbounded")
            last_boot_time = time
        boot_images.append({"attempt": attempt, "sequence": sequence, "presented_time": time,
                            "presentation": "displayed" if time else "unmeasured_or_skipped",
                            "sha256": hashlib.sha256(raw).hexdigest(), "source_sha256": hashlib.sha256(source).hexdigest()})
        previous_sequence, previous_count = sequence, count
    require(last_boot_time > 0 and int(ready[0]["submitted_present"]) == previous_count,
            "NGX visibility-ready bootstrap lacks actual displayed evidence")
    observations, previous_statistics = statistics_observations(ready, "actual_present")
    require(ready[0]["statistics"] == "actual", "NGX bootstrap readiness is unmeasured")
    images, times = [], []
    pacing_seconds = []
    for record, call, (frame, generated) in zip(drawables[len(bootstrap):], calls, expected, strict=True):
        require(float(call["phase"]) == (.5 if generated else 1.) and call.get("actual") == "1"
                and call.get("output_flag") == "0" and call.get("source") == "ngx11_gpu_copy_to_dxgi",
                "NGX public source phase/status mismatch")
        source = (fixture / f"fg-ngx-{frame}-{generated}.bin").read_bytes()
        require(len(source) == 128 * 128 * 4, "NGX GPU readback image size mismatch")
        if generated:
            require(source != (fixture / f"fg-ngx-{frame}-0.bin").read_bytes(),
                    "NGX generated phase repeats real frame")
        # Expected RGB comes from caller-authored geometry, not GPU readback or
        # adapter math. Stable checker interiors must survive interpolation;
        # current HUD is NOT interpolated. Real frames match every source pixel.
        probes = []
        for y in range(4, 128, 8):
            for x in range(4, 128, 4):
                checker = (((x + frame * 4) // 16) ^ (y // 16)) & 1
                rgb = (128 if checker else 64,
                       200 if 40 + frame * 2 < x < 64 + frame * 2 and 40 < y < 96 else 32,
                       32 if checker else 128)
                hud = x < 32 and y < 32
                if hud:
                    rgb = (32 + frame * 24, 220, 48)
                elif generated:
                    # Keep away from moving/occluding edges in BOTH history
                    # frames. Their interior colors are independently known.
                    history = frame - 1
                    if any((x + f * 4) % 16 < 4 or (x + f * 4) % 16 > 11
                           or min(abs(x - (40 + f * 2)), abs(x - (64 + f * 2))) < 8
                           for f in (history, frame)) or y % 16 < 4 or y % 16 > 11:
                        continue
                    if ((((x + history * 4) // 16) ^ (y // 16)) & 1) != checker:
                        continue
                i = (y * 128 + x) * 4
                require(all(abs(source[i + c] - rgb[c]) <= (6 if generated else 0) for c in range(3)),
                        f"NGX independent scene/HUD oracle mismatch frame={frame} generated={generated} pixel={x},{y}")
                probes.append((x, y))
        require(len(probes) >= 32, "insufficient independent NGX scene/HUD probes")
        sequence = int(record["sequence"])
        require(sequence > previous_sequence, "NGX native requests reordered")
        width, height, fmt = (int(record[k]) for k in ("width", "height", "format"))
        path = args.captures / Path(record["path"]).name
        require(not path.is_symlink() and path.is_file(), "NGX native capture is not a regular file")
        raw = path.read_bytes(); actual = rgba(raw, width, height, fmt)
        for x, y in probes:
            dx, dy = min(width - 1, int((x + .5) * width / 128)), min(height - 1, int((y + .5) * height / 128))
            si, di = (y * 128 + x) * 4, (dy * width + dx) * 4
            require(all(abs(actual[di + c] - source[si + c]) <= 3 for c in range(3)),
                    f"NGX native drawable content mismatch sequence={sequence} pixel={x},{y}")
        event = presented.get(sequence)
        require(event and event["drawable"] == record["drawable"], "NGX captured drawable lacks its native presented event")
        time = float(event["time"])
        require(math.isfinite(time) and time >= 0, "NGX formal frame has invalid native presentation timestamp")
        if time:
            require(time > last_boot_time, "NGX formal display precedes visibility bootstrap")
            if times:
                require(.001 <= time - times[-1] < 15, "NGX actual display timestamps lack paced ordered bounded progress")
            times.append(time)
        count, frequency = (int(call[k]) for k in ("submitted_present", "frequency"))
        require(count == previous_count + 1 and frequency == int(ready[0]["frequency"]),
                "NGX submitted DXGI requests are not sequential or use inconsistent clocks")
        pace_start, start, end = (int(call[k]) for k in ("pace_qpc_start", "qpc_start", "qpc_end"))
        require(0 < pace_start <= start <= end and end - pace_start < frequency * 15,
                "NGX caller output pacing/Present did not make bounded progress")
        pacing_seconds.append((start - pace_start) / frequency)
        sample, previous_statistics = statistics_observations([call], "actual_present", previous_statistics)
        observations.extend(sample)
        images.append({"frame": frame, "generated": generated, "sequence": sequence, "presented_time": time,
                       "phase": float(call["phase"]), "output_flag": int(call["output_flag"]),
                       "submitted_present": count, "presentation": "displayed" if time else "unmeasured_or_skipped",
                       "sha256": hashlib.sha256(raw).hexdigest(), "source_sha256": hashlib.sha256(source).hexdigest(),
                       "independent_scene_ui_probes": len(probes)})
        previous_sequence, previous_count = sequence, count
    visible = {(image["frame"], image["generated"]): image for image in images if image["presented_time"] > 0}
    require(all((frame, 0) in visible for frame in (2, 3, 4, 5, 6)), "NGX critical real frame lacks actual displayed evidence")
    require(all((frame, 1) in visible for frame in (2, 3, 4)),
            "NGX critical generated phase lacks actual displayed evidence")
    pairs = []
    for frame in range(1, 7):
        generated, real = visible.get((frame, 1)), visible.get((frame, 0))
        if generated and real:
            require(generated["presented_time"] < real["presented_time"], "NGX displayed generated/real pair reversed")
            pairs.append(frame)
    require(len(pairs) >= 3, "NGX repeated actual generated-before-real display is insufficient")
    require(sum(pacing_seconds) >= .001, "NGX caller WaitForVBlank pacing behaves as a success-only stub")
    drain = records(stdout, prefix + "_DRAIN")
    require(len(drain) == 1 and int(drain[0]["submitted_present"]) == previous_count
            and int(drain[0]["frequency"]) == int(ready[0]["frequency"]) and int(drain[0]["output_vblanks"]) == 3,
            "NGX final refresh drain/actual statistics incomplete")
    sample, previous_statistics = statistics_observations(drain, "actual_present", previous_statistics)
    observations.extend(sample)
    # DXGI_FRAME_STATISTICS.PresentCount is a displayed-frame count, not a
    # GetLastPresentCount/MTLDrawable sequence ID. Never join them as IDs.
    controls = {name: (fixture / name).read_bytes() for name in
                ("fg-ngx-fresh-4.bin", "fg-ngx-fresh-5.bin", "fg-ngx-replay-6.bin")}
    require(all(len(image) == 128 * 128 * 4 for image in controls.values()), "NGX history/reset/replay control image size mismatch")
    require(controls["fg-ngx-fresh-4.bin"] == controls["fg-ngx-fresh-5.bin"] == sentinel,
            "NGX fresh history bootstrap copied a fake generated frame")
    require((fixture / "fg-ngx-4-1.bin").read_bytes() != controls["fg-ngx-fresh-4.bin"],
            "NGX temporal history does not affect generated pixels")
    require((fixture / "fg-ngx-5-1.bin").read_bytes() == controls["fg-ngx-fresh-5.bin"],
            "NGX explicit reset did not preserve the output")
    require((fixture / "fg-ngx-6-1.bin").read_bytes() == controls["fg-ngx-replay-6.bin"],
            "NGX release-before-execute closed-list replay differs")
    return {"pass": True, "native_drawable_content_and_order": images, "attempted_drawables": len(expected),
            "positively_displayed_drawables": len(visible), "skipped_drawables": len(expected) - len(visible),
            "presentation_statistics_observations": observations,
            "physically_verified_generated_phases": [{"count": 1, "index": 1, "phase": .5, "frame": frame}
                                                      for frame in (2, 3, 4)],
            "suppressed_generated_outputs": suppressed,
            "bootstrap": {"attempted_drawables": len(bootstrap), "images": boot_images,
                          "positively_displayed_drawables": sum(image["presented_time"] > 0 for image in boot_images)},
            "displayed_generated_real_pairs": pairs, "caller_vblank_wait_seconds": pacing_seconds,
            "cadence_seconds": [b - a for a, b in zip(times, times[1:])],
            "timing_contract": "nine formal caller-owned DXGI Present(1) requests plus real bootstrap; latency=1, WaitForVBlank pacing; critical generated/real/reset outputs physically verified, legal skips explicit; statistics are latest counts, not per-id scanout acknowledgements",
            "compatibility_boundary": "NGX native public consumer, not real-game Streamline compatibility"}


def check(args):
    if getattr(args, "suite", "fsr-fg") == "ngx-fg-present":
        return check_ngx(args)
    stdout, stderr = args.stdout.read_text(), args.stderr.read_text()
    require("OWNED_D3D12_FSR_FG_PASS" in stdout, "public consumer incomplete")
    require("OWNED_D3D12_FSR_FG_OBSERVER_READY" in stderr, "native observer never loaded")
    require("OWNED_D3D12_FSR_FG_OBSERVER_FAIL" not in stderr, "native observer GPU failure")
    statistics = records(stdout, "OWNED_D3D12_FSR_FG_STATISTICS")
    finals = records(stdout, "OWNED_D3D12_FSR_FG_FINAL_STATISTICS")
    require([int(row["frame"]) for row in statistics] == [*range(1, 10), 11, 12, *range(11, 15)],
            "FSR statistics snapshot sequence incomplete/reordered")
    require([row["stream"] for row in finals] == ["callback", "builtin_ui"] and
            all(row["statistics"] == "actual" and int(row["output_vblanks"]) == 3 for row in finals),
            "FSR final refresh observations incomplete/unmeasured")
    observations = {}
    for stream, rows, final in zip(("callback", "builtin_ui"), (statistics[:11], statistics[11:]), finals, strict=True):
        require(all(row["source"] == "public_dxgi_actual_presentation" for row in [*rows, final]) and
                len({int(row["frequency"]) for row in [*rows, final]}) == 1, "FSR inconsistent statistics source/clock")
        submitted = [int(row["submitted_present"]) for row in [*rows, final]]
        require(all(a <= b for a, b in zip(submitted, submitted[1:])) and submitted[-1] == submitted[-2],
                "FSR submitted counts regressed or refresh drain submitted another frame")
        observations[stream], _ = statistics_observations([*rows, final], "native_present")
    callbacks = records(stdout, "OWNED_D3D12_FSR_FG_CALLBACK_PIXELS")
    counts = {2: 1, 3: 2, 4: 3, 8: 4}
    expected = []
    for frame in range(1, 13):
        expected.extend((frame, 1, index) for index in range(1, counts.get(frame, 0) + 1))
        if frame not in (8, 10, 12):
            expected.append((frame, 0, 0))
    require(int(finals[0]["submitted_present"]) == len(expected) and int(finals[1]["submitted_present"]) == 4,
            "FSR native submitted request count differs from validated outputs")
    require([(int(r["frame"]), int(r["generated"]), int(r["output_index"])) for r in callbacks] == expected,
            "public generated/real/OFF-ON/only-generated/callback-error sequence mismatch")
    drawables = records(stderr, "OWNED_D3D12_FSR_FG_DRAWABLE")
    events = records(stderr, "OWNED_D3D12_FSR_FG_PRESENTED")
    presented = {int(r["sequence"]): r for r in events}
    require(len(presented) == len(events), "duplicate native presented event")
    ui_frames = records(stdout, "OWNED_D3D12_FSR_FG_UI_FRAME")
    require([int(r["frame"]) for r in ui_frames] == [11, 12, 13, 14], "public wrap/UI/resize sequence incomplete")
    require(len(drawables) == len(expected) + 4, "native presentation count differs from validated outputs")
    require(len(events) == len(drawables), "native presented event count differs from captured requests")
    images = []
    previous_time = 0
    previous_sequence = 0
    phase_sources = {}
    for record, callback, (frame, generated, index) in zip(drawables[:len(expected)], callbacks, expected, strict=True):
        sequence = int(record["sequence"])
        require(sequence > previous_sequence, "native present requests reordered")
        width, height, fmt = (int(record[k]) for k in ("width", "height", "format"))
        path = args.captures / Path(record["path"]).name
        raw = path.read_bytes()
        actual = rgba(raw, width, height, fmt)
        source_path = args.stdout.parent / "fixture" / f"fg-callback-{frame}-{generated}-{index if generated else 1}.bin"
        source = source_path.read_bytes()
        sw, sh = int(callback["width"]), int(callback["height"])
        require(sw == sh == 128, "unexpected public callback dimensions")
        require(len(source) == sw * sh * 4, "public GPU readback image size mismatch")
        require(source[:4] == bytes((frame + 32, 190 if generated else 70, 220, 255)),
                "public source marker mismatch")
        content = bytearray(source)
        for y in range(8):
            content[y * sw * 4:y * sw * 4 + 32] = bytes(32)
        previous = phase_sources.setdefault(frame, [])
        require(content not in previous, "GPU phase repeats another phase or current color behind the UI marker")
        previous.append(content)
        # Probe marker interior AND a spatial grid of caller-authored scene
        # colors. Avoid resampling boundaries on Retina/scaled drawables.
        positions = [(2, 2), (4, 4)] + [(x, y) for y in (20, 36, 68, 100) for x in (20, 36, 68, 100)]
        for x, y in positions:
            dx = min(width - 1, int((x + .5) * width / sw))
            dy = min(height - 1, int((y + .5) * height / sh))
            si, di = (y * sw + x) * 4, (dy * width + dx) * 4
            require(all(abs(actual[di + c] - source[si + c]) <= 3 for c in range(3)),
                    f"native drawable content mismatch sequence={sequence} frame={frame} generated={generated} pixel={x},{y}")
        event = presented.get(sequence)
        require(event and event["drawable"] == record["drawable"], "captured drawable lacks native presented event")
        time = float(event["time"])
        # MTLDrawable explicitly returns zero for not-presented/skipped frames.
        # Preserve them; attempted presentation is not proof of display.
        require(math.isfinite(time) and time >= 0, "invalid native presentation timestamp")
        if time:
            require(time > previous_time and (not previous_time or time - previous_time < 15),
                    "actual presentation timestamps do not show ordered bounded progress")
        images.append({"sequence": sequence, "frame": frame, "generated": generated, "output_index": index,
                       "presentation": "displayed" if time else "unmeasured_or_skipped",
                       "presented_time": time, "sha256": hashlib.sha256(raw).hexdigest(),
                       "source_sha256": hashlib.sha256(source).hexdigest()})
        if time:
            previous_time = time
        previous_sequence = sequence
    visible = {(image["frame"], image["generated"], image["output_index"]): image for image in images if image["presented_time"] > 0}
    require(all((frame, 0, 0) in visible for frame in (2, 3, 4, 5, 6, 7, 9, 11)),
            "real, disabled or generation-error output lacks actual displayed evidence")
    missing = [(count, index) for frame, count in counts.items() for index in range(1, count + 1)
               if (frame, 1, index) not in visible]
    require(not missing, f"generated physical phase proof incomplete: unexercised count/index={missing}")
    pairs = []
    for frame in (2, 3, 4):
        generated = [visible[(frame, 1, index)] for index in range(1, counts[frame] + 1) if (frame, 1, index) in visible]
        real = visible.get((frame, 0, 0))
        if generated and real:
            require(all(image["presented_time"] < real["presented_time"] for image in generated), "displayed generated/real batch reversed")
            pairs.append(frame)
    require(len(pairs) >= 2, "repeated actual generated-before-real display not established")
    for record, frame in zip(drawables[len(expected):], range(11, 15), strict=True):
        sequence = int(record["sequence"]); width, height, fmt = (int(record[k]) for k in ("width", "height", "format"))
        require(sequence > previous_sequence, "wrapped native requests reordered")
        raw = (args.captures / Path(record["path"]).name).read_bytes()
        actual = rgba(raw, width, height, fmt)
        # Built-in SDK UI shader: premultiplied source-over in UNORM space.
        body = (frame * 8, 60, 100)
        overlay = (64 + frame * 8 * 127 / 255, 60 * 127 / 255, 100 * 127 / 255)
        for nx, ny, color in ((.02, .02, overlay), (.5, .5, body), (.8, .7, body)):
            x, y = min(width - 1, int(nx * width)), min(height - 1, int(ny * height))
            pixel = (y * width + x) * 4
            require(all(abs(actual[pixel + c] - color[c]) <= 3 for c in range(3)),
                    f"actual premultiplied/doublebuffer/resize output mismatch frame={frame}")
        event = presented.get(sequence)
        require(event and event["drawable"] == record["drawable"], "wrapped drawable lacks presented event")
        time = float(event["time"])
        require(math.isfinite(time) and previous_time < time < previous_time + 15, "wrapped actual timestamps not ordered/bounded")
        images.append({"sequence": sequence, "frame": frame, "generated": False, "premultiplied_ui": True,
                       "presentation": "displayed",
                       "presented_time": time, "sha256": hashlib.sha256(raw).hexdigest()})
        previous_time, previous_sequence = time, sequence
    require(not args.native, "MetalFX native pixel-equivalence is not the SDK phase algorithm; use native drawable content/order evidence")
    displayed = [image for image in images if image["presented_time"] > 0]
    return {"pass": True, "native_drawable_content_and_order": images,
            "attempted_drawables": len(images), "positively_displayed_drawables": len(displayed),
            "skipped_drawables": len(images) - len(displayed),
            "presentation_statistics_observations": observations,
            "physically_verified_generated_phases": [{"count": count, "index": index, "phase": index / (count + 1)}
                                                      for count in counts.values() for index in range(1, count + 1)],
            "displayed_generated_real_pairs": pairs,
            "gpu_generated_counts": counts, "algorithm": "genuine_sdk_normalized_phases",
            "cadence_seconds": [displayed[i]["presented_time"] - displayed[i - 1]["presented_time"]
                                                                 for i in range(1, len(displayed))],
            "timing_contract": "ordered bounded progress, not exact OS cadence"}


def main():
    parser = argparse.ArgumentParser()
    for name in ("stdout", "stderr", "captures", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--suite", choices=("fsr-fg", "ngx-fg-present"), default="fsr-fg")
    args = parser.parse_args()
    try:
        report = check(args)
    except (ValueError, KeyError, OSError) as error:
        report = {"pass": False, "error": str(error)}
    # Apple documents presentedTime==0 for not-presented/skipped drawables:
    # https://developer.apple.com/documentation/metal/mtldrawable/presentedtime
    # Preserve these even when a required physical phase makes the proof fail.
    try:
        prefix = "OWNED_D3D12_NGX_PRESENT" if args.suite == "ngx-fg-present" else "OWNED_D3D12_FSR_FG"
        events = records(args.stderr.read_text(), prefix + "_PRESENTED")
        report["native_skipped_drawables"] = [{"sequence": int(event["sequence"]), "drawable": event["drawable"],
                                              "presentation": "unmeasured_or_skipped"}
                                             for event in events if float(event["time"]) == 0]
    except (ValueError, KeyError, OSError):
        report["native_skipped_drawables"] = None
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    print(("OWNED_D3D12_NGX_PRESENT" if args.suite == "ngx-fg-present" else "OWNED_D3D12_FSR_FG") +
          "_CAPTURE_" + ("PASS" if report["pass"] else "FAIL"))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
