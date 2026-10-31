#!/usr/bin/env python3
"""CPU allocation-failure and signal-lifetime regression using production methods."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / "src/d3d12/d3d12_command_allocator.hpp").read_text()
allocator = (root / "src/d3d12/d3d12_command_allocator.cpp").read_text()
recording = (root / "src/d3d12/d3d12_command_list.cpp").read_text()
queue = (root / "src/d3d12/d3d12_command_queue.cpp").read_text()


def declaration(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    while True:
        depth = 1
        end = opening + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        remaining = source[end:].lstrip()
        if not remaining.startswith("catch"):
            return source[start:end]
        opening = source.index("{", end)


(root / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="command-recording-cpu-", dir=root / "build") as temporary:
    directory = Path(temporary)
    fragments = {
        "ARENA_POINTERS": header[header.index("constexpr auto kCPUHeapSize"):
                                 header.index("struct IndirectComputeCommandData")],
        "ARENA_METHODS": "\n".join((
            "void *\n" + declaration(header, "AllocateCPUHeap(size_t"),
            "template <typename T> T *\n" + declaration(header, "AllocatePass()"),
            "template <typename cmd_struct> cmd_struct &\n" + declaration(header, "EncodeComputeCommand()"),
            "HRESULT\n" + declaration(header, "EndRecord(size_t"),
            "HRESULT\n" + declaration(allocator, "MTLD3D12CommandAllocatorImpl::Initialize()")
                .replace("MTLD3D12CommandAllocatorImpl::", "", 1))),
        "RECORDING_METHODS": "\n".join(prefix + declaration(recording, marker) for prefix, marker in (
            ("bool ", "IsRecording() const"), ("void ", "RecordError(HRESULT"),
            ("HRESULT STDMETHODCALLTYPE ", "ValidateRecording()"),
            ("void STDMETHODCALLTYPE ", "Dispatch(UINT"),
            ("HRESULT STDMETHODCALLTYPE ", "Close()"),
            ("HRESULT STDMETHODCALLTYPE ", "TemporalUpscale(IMTLD3D12TemporalUpscaleState"))),
        "SIGNAL_BATCH": "void\n" + declaration(queue, "ProcessSignalBatch(std::vector"),
    }
    definitions = []
    for name, text in fragments.items():
        path = directory / (name.lower() + ".inc")
        path.write_text(text + "\n")
        definitions.append(f'-DPRODUCTION_{name}="{path}"')
    executable = directory / "command-recording-oom"
    command = ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
               *definitions, "graphics-translator/tests/command-recording-oom.test.cpp", "-o", str(executable)]
    subprocess.run(command, cwd=root, check=True)
    subprocess.run([str(executable)], cwd=root, check=True, timeout=60)
