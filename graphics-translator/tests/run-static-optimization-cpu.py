#!/usr/bin/env python3
"""Exercise production helper-cache and descriptor replacement methods with CPU stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def method(source: str, marker: str) -> str:
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


recording = (root / "src/d3d12/d3d12_command_list.cpp").read_text()
descriptors = (root / "src/d3d12/d3d12_descriptor_heap.cpp").read_text()
device = (root / "src/d3d12/d3d12_device.hpp").read_text()
(root / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="static-optimization-cpu-", dir=root / "build") as temporary:
    directory = Path(temporary)
    helper = directory / "helper.inc"
    helper.write_text(method(recording, "bool EnsurePredicationPipelines()") + "\n" +
                      method(recording, "WMT::Reference<WMT::ComputePipelineState> SharedCommandHelper(") + "\n")
    kinds = directory / "helper-kinds.inc"
    kinds.write_text(method(device, "enum class CommandHelper") + ";\n" +
                     method(device, "struct CommandHelperEntry") + ";\n" +
                     "std::array<CommandHelperEntry, size_t(CommandHelper::Count)> owned_command_helpers;\n")
    slots = directory / "slots.inc"
    slots.write_text(method(descriptors, "void ResetPoolSlot(UINT") + "\n" +
                     method(descriptors, "bool SetPoolSlot(UINT") + "\n")
    executable = directory / "static-optimization-cpu"
    subprocess.run([
        "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
        f'-DPRODUCTION_HELPER="{helper}"', f'-DPRODUCTION_SLOTS="{slots}"',
        f'-DPRODUCTION_HELPER_KINDS="{kinds}"',
        "graphics-translator/tests/static-optimization-cpu.test.cpp", "-o", str(executable),
    ], cwd=root, check=True)
    subprocess.run([str(executable)], cwd=root, check=True, timeout=60)
    sha1 = directory / "sha1.o"
    subprocess.run(["clang", "-std=c11", "-fsanitize=address,undefined", "-c",
                    "src/util/sha1/sha1.c", "-o", str(sha1)], cwd=root, check=True)
    for collisions in (False, True):
        bytecode = directory / ("bytecode-collision" if collisions else "bytecode")
        subprocess.run([
            "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
            *(["-DFORCE_DIGEST_COLLISION"] if collisions else []), "-Isrc/util", "-Isrc/d3d12",
            "graphics-translator/tests/immutable-shader-bytecode.test.cpp", "src/util/sha1/sha1_util.cpp",
            str(sha1), "-o", str(bytecode),
        ], cwd=root, check=True)
        subprocess.run([str(bytecode)], cwd=root, check=True, timeout=60)
    key = directory / "canonical-stage-key"
    subprocess.run([
        "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
        "-Isrc/util", "-Isrc/d3d12", "graphics-translator/tests/canonical-stage-key.test.cpp",
        "src/util/sha1/sha1_util.cpp", str(sha1), "-o", str(key),
    ], cwd=root, check=True)
    subprocess.run([str(key)], cwd=root, check=True, timeout=60)
    for name in ("temporal-submission-cpu.test", "owned-atomic64-storage", "async-pipeline-queue.test"):
        binary = directory / name
        subprocess.run([
            "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
            "-Isrc/d3d12",
            f"graphics-translator/tests/{name}.cpp", "-o", str(binary),
        ], cwd=root, check=True)
        subprocess.run([str(binary)], cwd=root, check=True, timeout=60)
