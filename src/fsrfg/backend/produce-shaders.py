#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Ordinary DXIL kernels; this producer never loads a graphics runtime.
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys

wine, dxc, prefix, output = map(pathlib.Path, sys.argv[1:5])
source = pathlib.Path(__file__).resolve().parent
output.mkdir(parents=True, exist_ok=True)
environment = dict(os.environ, WINEPREFIX=str(prefix.resolve()), WINEDEBUG='-all')
fi = ['reconstruct_and_dilate', 'setup', 'reconstruct_previous_depth', 'game_motion_vector_field', 'optical_flow_vector_field', 'disocclusion_mask', '', 'compute_inpainting_pyramid', 'inpainting', 'compute_game_vector_field_inpainting_pyramid', 'debug_view']
of = ['prepare_luma', 'compute_luminance_pyramid', 'generate_scd_histogram', 'compute_scd_divergence', 'compute_optical_flow_advanced_pass_v5', 'filter_optical_flow_pass_v5', 'scale_optical_flow_advanced_pass_v5']
cpp = ['// Generated from actual DXC resource reflection; do not edit.\n', '#include "framegeneration/fsr3/internal/ffx_frameinterpolation_shaderblobs.h"\n', '#include "framegeneration/fsr3/internal/ffx_opticalflow_shaderblobs.h"\n']
receipt = []

def windows(path):
    return 'Z:' + str(path.resolve())

def produce(kind, index, permutation, stem):
    name = f'{kind}{index}_p{permutation}'
    hlsl = 'ffx_frameinterpolation_bidirectional.hlsl' if kind == 'bi' else ('ffx_frameinterpolation' if kind == 'fi' else 'ffx_opticalflow') + ('_' + stem if stem else '') + ('.hlsl' if stem.endswith('v5') else '_pass.hlsl')
    input_path = source / 'framegeneration/fsr3/internal/shaders' / hlsl
    blob, assembly = output / (name + '.dxil'), output / (name + '.asm')
    arguments = ['-nologo', '-T', 'cs_6_2', '-E', 'CS', '-DFFX_HLSL=1', '-DFFX_GPU=1', '-DFFX_HALF=0', '-DFFX_HLSL_SM=62', '-DFFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL=0', '-Wno-for-redefinition', '-Wno-ambig-lit-shift']
    if kind in ['fi', 'bi']:
        arguments += ['-DFFX_FRAMEINTERPOLATION_EMBED_ROOTSIG=0', f'-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS={permutation & 1}', f'-DFFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS={(permutation >> 1) & 1}', f'-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH={(permutation >> 2) & 1}']
        if kind == 'bi':
            arguments += [f'-DDXMT_BI_PASS={index}']
    else:
        arguments += ['-DFFX_OPTICALFLOW_EMBED_ROOTSIG=0', f'-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT={permutation}']
    arguments += ['-I', windows(source / 'framegeneration/fsr3/include/gpu'), windows(input_path), '-Fo', windows(blob), '-Fc', windows(assembly)]
    result = subprocess.run([str(wine.resolve()), str(dxc.resolve()), *arguments], env=environment, capture_output=True, text=True)
    (output / (name + '.compile.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(name + ': ' + result.stderr)
    data = blob.read_bytes()
    receipt.append(dict(name=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), source_sha256=hashlib.sha256(input_path.read_bytes()).hexdigest(), arguments=arguments))
    cpp.append(f'#include "{name}.h"\nstatic FfxShaderBlob {name}_blob() {{ FfxShaderBlob b{{}}; b.data={name}; b.size={name}_len;\n')
    groups = {name: [] for name in ['ConstantBuffer', 'SRVTexture', 'UAVTexture', 'SRVBuffer', 'UAVBuffer', 'Sampler']}
    bindings = assembly.read_text().split('; Resource Bindings:')[1].split('target datalayout')[0]
    for line in bindings.splitlines():
        match = re.match(r';\s+(\w+)\s+(cbuffer|sampler|texture|UAV)\s+(\w+)\s+(\S+)\s+\w+\s+(cb|t|u|s)(\d+)\s+(\d+)', line)
        if not match:
            continue
        resource, typ, _, dimension, _, slot, count = match.groups()
        group = 'ConstantBuffer' if typ == 'cbuffer' else 'Sampler' if typ == 'sampler' else ('UAV' if typ == 'UAV' else 'SRV') + ('Buffer' if dimension in ['r/o', 'r/w', 'buf'] else 'Texture')
        groups[group].append((resource, int(slot), int(count)))
    if not any(groups.values()):
        raise RuntimeError(name + ': missing resource reflection')
    counters = dict(ConstantBuffer='cbvCount', SRVTexture='srvTextureCount', UAVTexture='uavTextureCount', SRVBuffer='srvBufferCount', UAVBuffer='uavBufferCount', Sampler='samplerCount')
    plural = dict(ConstantBuffer='ConstantBuffers', SRVTexture='SRVTextures', UAVTexture='UAVTextures', SRVBuffer='SRVBuffers', UAVBuffer='UAVBuffers', Sampler='Samplers')
    for group, rows in groups.items():
        if not rows:
            continue
        ident = name + '_' + group
        cpp.append(f'static const char* {ident}_names[]={{' + ','.join(json.dumps(row[0]) for row in rows) + '};\n')
        for label, column in [('slots', 1), ('counts', 2)]:
            cpp.append(f'static const uint32_t {ident}_{label}[]={{' + ','.join(str(row[column]) for row in rows) + '};\n')
        cpp.append(f'static const uint32_t {ident}_spaces[{len(rows)}]={{}};\nb.{counters[group]}={len(rows)}; b.bound{group}Names={ident}_names; b.bound{plural[group]}={ident}_slots; b.bound{group}Counts={ident}_counts; b.bound{group}Spaces={ident}_spaces;\n')
    cpp.append('return b; }\n')
    print(name, len(data), flush=True)

# Only preparation consumes low-resolution/jitter flags; depth atomic/mask
# passes consume inverted depth. Other passes consume prepared uniforms.
fi_masks = {0: 7, 2: 4, 5: 4}
bi_masks = {3: 4}
for index, stem in enumerate(fi):
    mask = fi_masks.get(index, 0)
    for permutation in range(8):
        if permutation & mask == permutation:
            produce('fi', index, permutation, stem)
for index in range(4):
    mask = bi_masks.get(index, 0)
    for permutation in range(8):
        if permutation & mask == permutation:
            produce('bi', index, permutation, '')
for permutation in range(2):
    for index, stem in enumerate(of):
        produce('of', index, permutation, stem)
cpp.append('extern "C" FfxErrorCode frameInterpolationGetPermutationBlobByIndex(FfxFrameInterpolationPass pass,uint32_t options,FfxShaderBlob* out) { switch(options & 7u) {\n')
for permutation in range(8):
    cpp.append(f'case {permutation}: switch(pass) {{\n')
    for index in range(15):
        kind, shader = ('fi', index) if index < 11 else ('bi', index - 11)
        variant = permutation & (fi_masks if kind == 'fi' else bi_masks).get(shader, 0)
        cpp.append(f'case {index}: *out={kind}{shader}_p{variant}_blob(); return FFX_OK;\n')
    cpp.append('default:return FFX_ERROR_INVALID_ENUM;}\n')
cpp.append('default:return FFX_ERROR_INVALID_ARGUMENT;} }\nextern "C" FfxErrorCode opticalflowGetPermutationBlobByIndex(FfxOpticalflowPass pass,uint32_t options,FfxShaderBlob* out) { switch((options >> 2) & 1u) {\n')
for permutation in range(2):
    cpp.append(f'case {permutation}: switch(pass) {{\n')
    for index in range(7):
        cpp.append(f'case {index}: *out=of{index}_p{permutation}_blob(); return FFX_OK;\n')
    cpp.append('default:return FFX_ERROR_INVALID_ENUM;}\n')
cpp.append('default:return FFX_ERROR_INVALID_ARGUMENT;} }\n')
(output / 'shaderblobs.cpp').write_text(''.join(cpp))
compiler = {file.name: hashlib.sha256(file.read_bytes()).hexdigest() for file in [dxc, dxc.parent / 'dxcompiler.dll', dxc.parent / 'dxil.dll']}
version = subprocess.run([str(wine.resolve()), str(dxc.resolve()), '--version'], env=environment, capture_output=True, text=True, check=True)
(output / 'provenance.json').write_text(json.dumps(dict(revision='0836aa7f24058b3f8d035a88c0774457d78fc98e', profile='cs_6_2', generic_fp32=True, forced_wave64=False, compiler_sha256=compiler, compiler_version=version.stdout.strip(), shaders=receipt), indent=2) + '\n')
