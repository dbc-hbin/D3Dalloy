#!/usr/bin/env python3
"""Cross-process native Metal4 archive GPU/optional-failure proof; fresh output required."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--out",type=Path,required=True)
args = parser.parse_args()
out = args.out.resolve(); out.mkdir(parents=True,exist_ok=False)
binary = out / "archive-native"
command=["xcrun","clang++","-std=c++20","-O2","-fblocks","-framework","Foundation",
    "-framework","Metal","graphics-translator/tests/owned-metal4-archive-native.mm","-o",str(binary)]
with (out / "build.log").open("w") as log:
    subprocess.run(command,cwd=root,stdout=log,stderr=subprocess.STDOUT,check=True)
sources=("src/winemetal/unix/winemetal_metal4_pipeline.c","src/winemetal/unix/metal4_pipeline_archive.inc",
         "src/winemetal/winemetal_metal4_pipeline.h","src/winemetal/winemetal.h",
         "graphics-translator/tests/owned-metal4-archive-native.mm","graphics-translator/tests/run-metal4-archive-native.py")
identity=dict(command=command,binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
              source_sha256={name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in sources},
              sdk=subprocess.check_output(["xcrun","--show-sdk-path"],text=True).strip())
env = dict(os.environ,D3DALLOY_METAL4_ARCHIVE_DIR=str(out / "cache"),D3DALLOY_METAL4_ARCHIVE_TRACE="1",
           MTL_DEBUG_LAYER="1",MTL_SHADER_VALIDATION="0",WINEPREFIX=str(out / "native-only-identity"))
rows = []
def run(label,mode,extra=None):
    path=out/(label+".log")
    environment=dict(env,**(extra or {}))
    with path.open("w") as log:
        result=subprocess.run([str(binary),mode],cwd=root,env=environment,stdout=log,stderr=subprocess.STDOUT,timeout=120)
    text=path.read_text()
    passed=result.returncode==0 and f"METAL4_ARCHIVE_PASS mode={mode}" in text and not any(
        marker in text for marker in ("METAL4_ARCHIVE_FAIL","Shader Validation Error","failed assertion","kIOGPUCommandBufferCallbackError"))
    rows.append(dict(case=label,exit=result.returncode,passed=passed,log=path.name,
                     api_validation=True,shader_validation=environment["MTL_SHADER_VALIDATION"]=="1"))
    (out/"result.json").write_text(json.dumps(dict(identity=identity,cases=rows),indent=2))
    print(label,result.returncode,passed,flush=True)
    if not passed: raise RuntimeError(text[-8000:])
run("cold","cold")
files=list((out/"cache").rglob("*.mtl4archive"));assert files
run("warm","warm")
run("controls","controls")
files=list((out/"cache").rglob("*.mtl4archive"));assert files
corrupted=[]
for file in files:
    corrupted.append(dict(path=str(file.relative_to(out)),sha256=hashlib.sha256(file.read_bytes()).hexdigest()))
    file.write_bytes(b"not-a-metal-archive")
(out/"corruption-input.json").write_text(json.dumps(corrupted,indent=2))
run("corrupt-fallback","cold")
run("disabled","cold",dict(D3DALLOY_METAL4_ARCHIVE="0"))
bad=out/"not-a-directory";bad.write_text("preserve this file\n")
run("invalid-directory","cold",dict(D3DALLOY_METAL4_ARCHIVE_DIR=str(bad)))
assert bad.read_text()=="preserve this file\n"
run("shader-validation-fallback","validation-fallback",dict(MTL_SHADER_VALIDATION="1"))
print("METAL4_ARCHIVE_NATIVE_ALL_PASS")
