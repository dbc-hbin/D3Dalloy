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


def check(args):
    stdout, stderr = args.stdout.read_text(), args.stderr.read_text()
    require("OWNED_D3D12_FSR_FG_PASS" in stdout, "public consumer incomplete")
    require("OWNED_D3D12_FSR_FG_OBSERVER_READY" in stderr, "native observer never loaded")
    require("OWNED_D3D12_FSR_FG_OBSERVER_FAIL" not in stderr, "native observer GPU failure")
    callbacks = records(stdout, "OWNED_D3D12_FSR_FG_CALLBACK_PIXELS")
    expected = [(frame, generated) for frame in range(1, 10)
                for generated in ((1, 0) if frame in (1, 2, 3, 4, 7) else (1,) if frame == 8 else (0,))]
    require([(int(r["frame"]), int(r["generated"])) for r in callbacks] == expected,
            "public generated/real/OFF-ON/only-generated/callback-error sequence mismatch")
    drawables = records(stderr, "OWNED_D3D12_FSR_FG_DRAWABLE")
    events = records(stderr, "OWNED_D3D12_FSR_FG_PRESENTED")
    presented = {int(r["sequence"]): r for r in events}
    require(len(presented) == len(events), "duplicate native presented event")
    ui_frames = records(stdout, "OWNED_D3D12_FSR_FG_UI_FRAME")
    require([int(r["frame"]) for r in ui_frames] == [11, 12, 13, 14], "public wrap/UI/resize sequence incomplete")
    require(len(drawables) == len(expected) + 4, "native presentation count differs from validated outputs")
    images = []
    previous_time = 0
    previous_sequence = 0
    for record, callback, (frame, generated) in zip(drawables[:len(expected)], callbacks, expected, strict=True):
        sequence = int(record["sequence"])
        require(sequence > previous_sequence, "native present requests reordered")
        width, height, fmt = (int(record[k]) for k in ("width", "height", "format"))
        path = args.captures / Path(record["path"]).name
        raw = path.read_bytes()
        actual = rgba(raw, width, height, fmt)
        source_path = args.stdout.parent / "fixture" / f"fg-callback-{frame}-{generated}.bin"
        source = source_path.read_bytes()
        sw, sh = int(callback["width"]), int(callback["height"])
        require(sw == sh == 128, "unexpected public callback dimensions")
        require(len(source) == sw * sh * 4, "public GPU readback image size mismatch")
        require(source[:4] == bytes((frame + 32, 190 if generated else 70, 220, 255)),
                "public source marker mismatch")
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
        images.append({"sequence": sequence, "frame": frame, "generated": generated,
                       "presentation": "displayed" if time else "not_displayed_or_skipped",
                       "presented_time": time, "sha256": hashlib.sha256(raw).hexdigest(),
                       "source_sha256": hashlib.sha256(source).hexdigest()})
        if time:
            previous_time = time
        previous_sequence = sequence
    visible = {(image["frame"], image["generated"]): image for image in images if image["presented_time"] > 0}
    require(all((frame, 0) in visible for frame in (1, 2, 3, 4, 5, 6, 7, 9)),
            "real, disabled or generation-error output lacks actual displayed evidence")
    require((8, 1) in visible, "only-generated output was not actually displayed")
    pairs = []
    for frame in (1, 2, 3, 4, 7):
        generated, real = visible.get((frame, 1)), visible.get((frame, 0))
        if generated and real:
            require(generated["presented_time"] < real["presented_time"], "displayed generated/real pair reversed")
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
    native = []
    if args.native:
        for frame in range(1, 5):
            a = (args.stdout.parent / "fixture" / f"fg-direct-frame-{frame}.bin").read_bytes()
            b = (args.native / f"native-frame-{frame}.bin").read_bytes()
            require(len(a) == len(b) == 128 * 128 * 8, "independent native control image size mismatch")
            errors = [abs(x[0] - y[0]) for x, y in zip(struct.iter_unpack("<e", a), struct.iter_unpack("<e", b), strict=True)]
            require(all(math.isfinite(e) and e <= .003 for e in errors), f"public native MetalFX control mismatch frame={frame}")
            native.append({"frame": frame, "max_abs_error": max(errors), "consumer_sha256": hashlib.sha256(a).hexdigest(),
                           "native_sha256": hashlib.sha256(b).hexdigest()})
    displayed = [image for image in images if image["presented_time"] > 0]
    return {"pass": True, "native_drawable_content_and_order": images,
            "attempted_drawables": len(images), "positively_displayed_drawables": len(displayed),
            "displayed_generated_real_pairs": pairs,
            "public_metalfx_control": native, "cadence_seconds": [displayed[i]["presented_time"] - displayed[i - 1]["presented_time"]
                                                                 for i in range(1, len(displayed))],
            "timing_contract": "ordered bounded progress, not exact OS cadence"}


def main():
    parser = argparse.ArgumentParser()
    for name in ("stdout", "stderr", "captures", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--native", type=Path)
    args = parser.parse_args()
    try:
        report = check(args)
    except (ValueError, OSError) as error:
        report = {"pass": False, "error": str(error)}
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    print("OWNED_D3D12_FSR_FG_CAPTURE_" + ("PASS" if report["pass"] else "FAIL"))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
