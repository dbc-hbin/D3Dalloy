#!/usr/bin/env python3
"""GPU checks for recovered RR execution; never an NVIDIA image-parity claim."""
import argparse
from bisect import bisect_left
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
from time import perf_counter

import mlx.core as mx
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
RECOVERY = ROOT / 'build/local/dlss-rr-extraction-20261011/reconstruction'
sys.path.insert(0, str(RECOVERY / 'cuda'))
from transformer_reference import cubic_pair
from embedding_reference import value, rounded
from rr_mlx.runtime import check_metadata


def primitives(output):
    header = (Path(__file__).parent / 'rr_mlx/ops.metal').read_text()
    kernel = mx.fast.metal_kernel(name='rr_primitive_half', input_names=['bits'],
        output_names=['encoded', 'activated', 'cubic', 'fma_values'], header=header,
        compile_options={'math_mode': 'safe'}, source='''
uint i=thread_position_in_grid.x; if(i>=65536u) return;
uint h=bits[i], pair=h|((h^0x8000u)<<16);
encoded[i]=rr_fp8x2(pair,false); activated[i]=rr_fp8x2(pair,true);
uint x=rr_half2_mul(pair,0x24662466u);
float2 v=clamp(float2(rr_half_float(x),rr_half_float(x>>16)),
               float2(rr_half_float(0xb873u)),float2(rr_half_float(0x3873u)));
x=rr_half_bits(v.x)|(rr_half_bits(v.y)<<16);
uint p=rr_half2_fma(x^0x80008000u,x,0x3b6b3b6bu);
uint q=rr_half2_fma(x,p,0x3d803d80u);
cubic[i]=(q<<5)&0x7fe07fe0u;
fma_values[i]=rr_half_fma(0x3c01u,0x3e00u,h);
''')
    actual = kernel(inputs=[mx.arange(65536, dtype=mx.uint32)], grid=(65536, 1, 1),
                    threadgroup=(256, 1, 1), output_shapes=[(65536,)] * 4,
                    output_dtypes=[mx.uint32] * 4)
    mx.eval(*actual)
    encoded, activated, cubic, fma_values = [np.asarray(a) for a in actual]
    values = [i * 2**-9 if i < 8 else (1 + (i & 7) / 8) * 2**((i >> 3) - 7) for i in range(127)]
    def expected_fp8(h, relu):
        x = struct.unpack('<e', struct.pack('<H', h))[0]
        if math.isnan(x):
            return 127
        if relu and x < 0:
            return 0
        sign = (h >> 8) & 128
        x = abs(x)
        index = bisect_left(values, x)
        candidates = {min(index, 126), max(0, index - 1)}
        nearest = min(candidates, key=lambda k: (abs(values[k] - x), k & 1))
        return sign | nearest
    finite = 0
    for h in range(65536):
        pair = (h, h ^ 32768)
        for relu, observed in ((False, encoded), (True, activated)):
            expected = expected_fp8(pair[0], relu) | (expected_fp8(pair[1], relu) << 8)
            assert int(observed[h]) == expected, ('fp8', h, relu, int(observed[h]), expected)
        if h & 0x7c00 == 0x7c00:
            continue
        assert int(cubic[h]) == cubic_pair(pair), ('cubic', h, int(cubic[h]), cubic_pair(pair))
        expected = rounded(value(0x3c01) * value(0x3e00) + value(h))
        assert int(fma_values[h]) == expected, ('fma', h, int(fma_values[h]), expected)
        finite += 1
    assert int(fma_values[1]) == 0x3e02 and int(fma_values[0x8001]) == 0x3e01
    # Independent matrix basis -> PTX fragment packing and row/column oracle.
    count = 512 + 256 + 128
    a = np.zeros((count, 32, 4), dtype=np.uint32)
    b = np.zeros((count, 32, 2), dtype=np.uint32)
    c = np.zeros((count, 32, 2), dtype=np.uint32)
    expected = np.zeros((count, 16, 8), dtype=np.float16)
    b[:512] = 0x38383838
    a[512:768] = 0x38383838
    for case in range(512):
        row, k = divmod(case, 32)
        lane = (row % 8) * 4 + (k % 16) // 4
        register = row // 8 + 2 * (k // 16)
        a[case, lane, register] = 0x38 << (8 * (k % 4))
        expected[case, row, :] = 1
    for case in range(256):
        k, col = divmod(case, 8)
        lane = col * 4 + (k % 16) // 4
        b[512 + case, lane, k // 16] = 0x38 << (8 * (k % 4))
        expected[512 + case, :, col] = 1
    for case in range(128):
        row, col = divmod(case, 8)
        lane = (row % 8) * 4 + col // 2
        c[768 + case, lane, row // 8] = 0x3c00 << (16 * (col % 2))
        expected[768 + case, row, col] = 1
    mma = mx.fast.metal_kernel(name='rr_primitive_mma', input_names=['a', 'b', 'c'],
        output_names=['d', 'width'], header=header, compile_options={'math_mode': 'safe'}, source='''
uint i=thread_position_in_grid.x, lane=thread_index_in_simdgroup;
uint2 result=rr_mma(uint4(a[i*4],a[i*4+1],a[i*4+2],a[i*4+3]),
                    uint2(b[i*2],b[i*2+1]),uint2(c[i*2],c[i*2+1]),lane);
d[i*2]=result.x; d[i*2+1]=result.y; width[i]=threads_per_simdgroup;
''')
    d, width = mma(inputs=[mx.array(x.reshape(-1)) for x in (a, b, c)],
                   grid=(count * 32, 1, 1), threadgroup=(32, 1, 1),
                   output_shapes=[(count * 64,), (count * 32,)], output_dtypes=[mx.uint32, mx.uint32])
    mx.eval(d, width)
    assert np.all(np.asarray(width) == 32)
    raw = np.asarray(d).reshape(count, 32, 2)
    restored = np.empty_like(expected)
    for row in range(16):
        for col in range(8):
            bits = (raw[:, (row % 8) * 4 + col // 2, row // 8] >> (16 * (col % 2))).astype(np.uint16)
            restored[:, row, col] = bits.view(np.float16)
    assert np.array_equal(restored, expected), ('MMA basis', np.argwhere(restored != expected)[:8].tolist())
    result = {'pass': True, 'device': mx.device_info(), 'metadata': check_metadata(),
              'fp8_half_patterns': 65536, 'fp8_relu': True, 'half_cubic_cases': finite,
              'half_fma_cases': finite, 'mma_basis_cases': count, 'simd_width': 32,
              'ops_sha256': hashlib.sha256(header.encode()).hexdigest(),
              'limits': ['NVIDIA MMA accumulation order and SFU bit identity are not established.',
                         'This primitive result alone is not complete model execution.']}
    output.mkdir(parents=True, exist_ok=True)
    (output / 'primitives.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    return result


def instruction_edges(output):
    header = (Path(__file__).parent / 'rr_mlx/ops.metal').read_text()
    kernel = mx.fast.metal_kernel(name='rr_instruction_edges', input_names=['dummy'],
        output_names=['result'], header=header, compile_options={'math_mode': 'safe'}, source='''
uint lane=thread_index_in_simdgroup, row=lane>>2, col=(lane&3)*2;
uint v=(8*row+col)|((8*row+col+1)<<16);
result[lane*10]=rr_movmatrix(v,lane);
result[lane*10+1]=rr_half_bits_rz(1.0009f);
result[lane*10+2]=rr_half_bits_rz(1e10f);
result[lane*10+3]=as_type<uint>(rr_add_rz(1.f,-0x1p-25f));
result[lane*10+4]=as_type<uint>(rr_add_rz(1.f,0x1p-25f));
result[lane*10+5]=rr_shuffle(lane,lane,1,0,0xffffffffu,0).x;
result[lane*10+6]=rr_shuffle(lane,lane,1,31,0xffffffffu,1).x;
result[lane*10+7]=rr_shuffle(lane,lane,1,0x181f,0xffffffffu,2).x;
result[lane*10+8]=rr_shuffle(lane,lane,3,0x181f,0xffffffffu,3).x;
result[lane*10+9]=rr_shuffle(lane,lane,0,31,0xfffffffeu,3).y;
''')
    result, = kernel(inputs=[mx.array([0], dtype=mx.uint32)], grid=(32, 1, 1), threadgroup=(32, 1, 1),
                     output_shapes=[(320,)], output_dtypes=[mx.uint32])
    actual = np.asarray(result).reshape(32, 10)
    for lane in range(32):
        row, col = lane // 4, (lane % 4) * 2
        expected = [(8 * col + row) | ((8 * (col + 1) + row) << 16), 0x3c00, 0x7bff,
                    0x3f7fffff, 0x3f800000, max(lane - 1, 0), min(lane + 1, 31),
                    lane ^ 1, (lane & 24) | 3, 1]
        assert actual[lane].tolist() == expected, ('instruction edges', lane, actual[lane].tolist(), expected)
    x = np.exp2(np.linspace(-120, 120, 2048)).astype(np.float32)
    exponent = np.linspace(-120, 120, 2048, dtype=np.float32)
    sfu = mx.fast.metal_kernel(name='rr_instruction_sfu', input_names=['x', 'exponent'], output_names=['y'],
        header=header, compile_options={'math_mode': 'safe'}, source='''
uint i=thread_position_in_grid.x; if(i>=2048) return;
y[i*5]=rr_sfu(exponent[i],256u);
for(uint op=1;op<=4;++op) y[i*5+op]=rr_sfu(x[i],op|256u);
''')
    y, = sfu(inputs=[mx.array(x), mx.array(exponent)], grid=(2048, 1, 1), threadgroup=(256, 1, 1),
             output_shapes=[(2048 * 5,)], output_dtypes=[mx.float32])
    actual = np.asarray(y).reshape(-1, 5).astype(np.float64)
    exact = np.stack([np.exp2(exponent.astype(np.float64)), np.log2(x.astype(np.float64)), 1 / x.astype(np.float64),
                      1 / np.sqrt(x.astype(np.float64)), np.sqrt(x.astype(np.float64))], axis=1)
    error = abs(actual - exact)
    bounds = np.stack([2 * np.spacing(exact[:, 0].astype(np.float32)).astype(np.float64),
                       np.maximum(abs(exact[:, 1]), 1) * 2**-22,
                       np.spacing(exact[:, 2].astype(np.float32)).astype(np.float64),
                       abs(exact[:, 3]) * 2**-22.9, abs(exact[:, 4]) * 2**-23], axis=1)
    assert np.all(error <= bounds), ('SFU public bounds', np.argwhere(error > bounds)[:8].tolist())
    report = {'pass': True, 'movmatrix_lanes': 32, 'shuffle_lanes_modes': 128, 'rz_cases': 4,
              'sfu_inputs': 2048, 'sfu_ops': ['ex2', 'lg2', 'rcp', 'rsqrt', 'sqrt'],
              'max_bound_fraction': (error / bounds).max(axis=0).tolist(),
              'ops_sha256': hashlib.sha256(header.encode()).hexdigest()}
    output.mkdir(parents=True, exist_ok=True)
    (output / 'instruction-edges.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


def compiler(output):
    from rr_mlx import ptx, runtime

    source = '''.version 8.7
.target sm_89
.address_size 64
.visible .entry check(.param .align 8 .b8 argument[8]) {
.reg .b32 %r<12>; .reg .b64 %rd<4>; .reg .pred %p<2>; .reg .f32 %f<4>;
.shared .align 4 .b8 tile[128];
ld.param.u64 %rd0, [argument];
mov.u32 %r0, %tid.x; mov.u32 %r1, %ctaid.x;
setp.lt.u32 %p0, %r0, 16;
@%p0 bra low;
add.s32 %r2, %r0, 10; bra join;
low: add.s32 %r2, %r0, 100;
join: mad.lo.s32 %r2, %r1, 1000, %r2;
mov.u32 %r3, tile; shl.b32 %r4, %r0, 2; add.s32 %r3, %r3, %r4;
st.shared.u32 [%r3], %r2;
bar.sync 0;
ld.shared.u32 %r5, [%r3];
sub.s32 %r6, 31, %r0;
shfl.sync.idx.b32 %r7, %r5, %r6, 31, -1;
vote.sync.ballot.b32 %r8, %p0, -1;
mad.lo.s32 %r9, %r1, 32, %r0; mul.wide.u32 %rd1, %r9, 8;
add.s64 %rd2, %rd0, %rd1;
st.global.v2.u32 [%rd2], {%r7,%r8};
ret;
}'''
    spec = {'schema': 'rr.mlx.dispatch.v1', 'entry': 'check', 'module': 'independent-fixture',
            'model_selector': 5, 'params': struct.pack('<Q', 4096),
            'cuda_grid': [2, 1, 1], 'cuda_block': [32, 1, 1], 'global_symbols': {},
            'resources': [{'resource_id': 1, 'handle': 4096, 'virtual_base': 4096,
                           'arena_offset': 64, 'bytes': 512, 'buffer': 'arena',
                           'access': 'readwrite', 'view': {'kind': 'global'}}],
            'state_before': {}, 'source_locations': []}
    original = np.full(640, 0xa5, np.uint8)
    arena = mx.array(original)
    actual, receipt = runtime.execute(spec, ptx.compile_kernel(source, 'check', spec), arena, b'', b'')
    result = np.asarray(actual)
    np.testing.assert_array_equal(np.asarray(arena), original)
    np.testing.assert_array_equal(result[:64], original[:64])
    np.testing.assert_array_equal(result[-64:], original[-64:])
    lanes = np.arange(32, dtype=np.uint32)
    expected = np.stack([np.tile(np.where(lanes < 16, lanes + 100, lanes + 10)[::-1], 2)
                         + np.repeat(np.array([0, 1000], np.uint32), 32), np.full(64, 65535, np.uint32)], axis=1)
    np.testing.assert_array_equal(result[64:-64].view(np.uint32).reshape(64, 2), expected)
    exited = source.replace('setp.lt.u32 %p0, %r0, 16;', 'setp.lt.u32 %p0, %r0, 16; @%p0 ret;')
    exited = exited.replace('sub.s32 %r6, 31, %r0;', 'mov.u32 %r6, %r0;')
    actual, exited_receipt = runtime.execute(spec, ptx.compile_kernel(exited, 'check', spec), arena, b'', b'')
    expected_exited = original.copy()
    rows = expected_exited[64:-64].view(np.uint32).reshape(2, 32, 2)
    rows[:, 16:, 0] = lanes[16:] + 10 + np.array([0, 1000], np.uint32)[:, None]
    rows[:, 16:, 1] = 0
    np.testing.assert_array_equal(np.asarray(actual), expected_exited)
    cropped = '''.version 8.7
.target sm_89
.address_size 64
.visible .entry check(.param .align 8 .b8 argument[8]) {
.reg .b32 %r<12>; .reg .b64 %rd<4>; .reg .pred %p<2>;
ld.param.u64 %rd0, [argument]; mov.u32 %r0, %tid.x;
and.b32 %r2, %r0, 22; setp.eq.u32 %p0, %r2, 22;
vote.sync.ballot.b32 %r3, %p0, -1;
mov.u32 %r7, 0;
@!%p0 bra cropped_join;
shfl.sync.down.b32 %r4, %r0, 1, 0x1e1f, %r3;
add.s32 %r5, %r0, %r4;
shfl.sync.down.b32 %r6, %r5, 8, 0x171f, %r3;
add.s32 %r7, %r5, %r6;
cropped_join:
vote.sync.ballot.b32 %r8, %p0, -1;
mul.wide.u32 %rd1, %r0, 8; add.s64 %rd2, %rd0, %rd1;
st.global.v2.u32 [%rd2], {%r7,%r8}; ret;
}'''
    crop_spec = {**spec, 'cuda_grid': [1, 1, 1]}
    actual, crop_receipt = runtime.execute(crop_spec, ptx.compile_kernel(cropped, 'check', crop_spec), arena, b'', b'')
    expected_crop = original.copy()
    rows = expected_crop[64:320].view(np.uint32).reshape(32, 2)
    rows[:, 0] = 0
    rows[:, 1] = 0xc0c00000
    rows[[22, 23, 30, 31], 0] = [106, 108, 122, 124]
    np.testing.assert_array_equal(np.asarray(actual), expected_crop)
    faults = []
    cases = [('global OOB', 1, source, {**spec, 'params': struct.pack('<Q', 8192)}),
             ('shared before write', 128, source.replace('st.shared.u32 [%r3], %r2;', ''), spec),
             ('missing texture', 4, source.replace('ret;', 'tex.base.2d.v4.f32.s32 {%f0,%f1,%f2,%f3}, [999, {%r0,%r0}]; ret;'), spec),
             ('undefined shuffle source', 8, source.replace('%r6, 31, -1;', '%r6, 31, 65535;'), spec),
             ('trap', 16, source.replace('ret;', 'trap; ret;'), spec)]
    for name, bit, text, dispatch in cases:
        try:
            runtime.execute(dispatch, ptx.compile_kernel(text, 'check', dispatch), arena, b'', b'')
        except runtime.DispatchError as error:
            assert error.diagnostics['raw'][0] == bit, error.diagnostics
            faults.append({'case': name, 'raw': error.diagnostics['raw']})
        else:
            raise AssertionError(f'{name}: missing required GPU diagnostic')
    report = {'pass': True, 'serial_ctas': 2, 'immutable_input_and_canaries': True,
              'branch_shared_shuffle_ballot': receipt, 'non_exited_shuffle_ballot': exited_receipt,
              'cropped_pairs_rejoin_ballot': crop_receipt,
              'diagnostics': faults}
    output.mkdir(parents=True, exist_ok=True)
    (output / 'compiler.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'pass': True, 'compiler_scenarios': 8}))


def views(output):
    from rr_mlx import ptx, runtime

    source = '''.version 8.7
.target sm_89
.address_size 64
.visible .entry views(.param .align 8 .b8 argument[24]) {
.reg .b32 %r<1>; .reg .b64 %rd<5>; .reg .pred %p<1>; .reg .f32 %f<9>; .reg .b16 %h<1>;
mov.u32 %r0, %tid.x; setp.ne.u32 %p0, %r0, 0; @%p0 ret;
ld.param.u64 %rd0, [argument]; ld.param.u64 %rd1, [argument+8]; ld.param.u64 %rd2, [argument+16];
tex.2d.v4.f32.f32 {%f0,%f1,%f2,%f3}, [%rd0, {0f3f000000,0f3f000000}];
tld4.r.2d.v4.f32.f32 {%f4,%f5,%f6,%f7}, [%rd0, {0f3f000000,0f3f000000}];
st.global.v4.f32 [%rd2], {%f0,%f1,%f2,%f3};
add.s64 %rd3, %rd2, 16; st.global.v4.f32 [%rd3], {%f4,%f5,%f6,%f7};
sust.p.2d.v4.b32.zero [%rd1, {0,1}], {%f0,%f1,%f2,%f3};
sust.p.2d.v4.b32.clamp [%rd1, {5,-1}], {%f4,%f5,%f6,%f7};
sust.p.2d.v4.b32.zero [%rd1, {2,1}], {%f4,%f5,%f6,%f7};
mov.b16 %h0, 14336; sust.b.2d.b16.zero [%rd1, {0,0}], %h0;
suld.b.2d.b16.zero %h0, [%rd1, {2,0}]; cvt.f32.f16 %f8, %h0;
add.s64 %rd4, %rd2, 32; st.global.f32 [%rd4], %f8;
ret;
}'''
    texture = {'resource_id': 1, 'handle': 1, 'virtual_base': 4096, 'arena_offset': 64,
               'bytes': 32, 'buffer': 'arena', 'access': 'read', 'shape': [2, 2, 1, 1],
               'storage': {'dtype': 'f16', 'row_pitch': 16}, 'view': {'mode': 0}}
    surface = {**texture, 'resource_id': 2, 'handle': 2, 'virtual_base': 8192,
               'arena_offset': 96, 'access': 'readwrite'}
    result_span = {'resource_id': 3, 'handle': 3, 'virtual_base': 12288, 'arena_offset': 128,
                   'bytes': 64, 'buffer': 'arena', 'access': 'readwrite', 'view': {'kind': 'global'}}
    spec = {'schema': 'rr.mlx.dispatch.v1', 'entry': 'views', 'module': 'independent-pitched-views',
            'model_selector': 5, 'params': struct.pack('<QQQ', 1, 2, 12288), 'cuda_grid': [1, 1, 1],
            'cuda_block': [32, 1, 1], 'global_symbols': {}, 'resources': [texture, surface, result_span],
            'state_before': {}, 'source_locations': []}
    original = np.full(256, 0xa5, np.uint8)
    original[64:68] = np.array([1, 2], np.float16).view(np.uint8)
    original[80:84] = np.array([3, 4], np.float16).view(np.uint8)
    expected = original.copy()
    expected[128:164] = np.array([2.5, 0, 0, 1, 3, 4, 2, 1, 3], np.float32).view(np.uint8)
    expected[96:100] = np.array([0.5, 3], np.float16).view(np.uint8)
    expected[112:114] = np.array([2.5], np.float16).view(np.uint8)
    arena = mx.array(original)
    actual, receipt = runtime.execute(spec, ptx.compile_kernel(source, 'views', spec), arena, b'', b'')
    np.testing.assert_array_equal(np.asarray(actual), expected)
    np.testing.assert_array_equal(np.asarray(arena), original)
    report = {'pass': True, 'linear_gather_defaults': True, 'formatted_extra_components_ignored': True,
              'raw_byte_addressing': True, 'clamp_zero_boundaries': True, 'pitch_canaries_and_input': True,
              'receipt': receipt}
    output.mkdir(parents=True, exist_ok=True)
    (output / 'views.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


def binary64(output):
    rng = np.random.default_rng(64)
    a = rng.integers(0, 1 << 64, 8192, dtype=np.uint64)
    b = rng.integers(0, 1 << 64, 8192, dtype=np.uint64)
    edges = np.array([0, 1, 0x000fffffffffffff, 0x0010000000000000,
                      0x3ff0000000000000, 0x7fefffffffffffff,
                      0x7ff0000000000000, 0x7ff8000000000000,
                      0x8000000000000000, 0xbff0000000000000,
                      0xfff0000000000000], dtype=np.uint64)
    a[:len(edges) ** 2] = np.repeat(edges, len(edges))
    b[:len(edges) ** 2] = np.tile(edges, len(edges))
    c = rng.integers(0, 1 << 32, 8192, dtype=np.uint32)
    c[:9] = [0, 1, 0x007fffff, 0x00800000, 0x3f800000, 0x7f7fffff,
             0x7f800000, 0x7fc00000, 0x80000000]
    header = (ROOT / 'scripts/rr_mlx/ops.metal').read_text()
    kernel = mx.fast.metal_kernel(name='rr_binary64_check', input_names=['a', 'b', 'c'],
        output_names=['division', 'maximum', 'narrow', 'wide'], header=header,
        compile_options={'math_mode': 'safe'},
        source='uint i=thread_position_in_grid.x; division[i]=rr_f64_div(a[i],b[i]);'
               'maximum[i]=rr_f64_max(a[i],b[i]); narrow[i]=as_type<uint>(rr_f64_to_f32(a[i]));'
               'wide[i]=rr_f32_to_f64(as_type<float>(c[i]));')
    outputs = kernel(inputs=[mx.array(a), mx.array(b), mx.array(c)], grid=(8192, 1, 1),
                     threadgroup=(32, 1, 1), output_shapes=[(8192,)] * 4,
                     output_dtypes=[mx.uint64, mx.uint64, mx.uint32, mx.uint64])
    mx.eval(*outputs)
    with np.errstate(all='ignore'):
        af, bf = a.view(np.float64), b.view(np.float64)
        # PTX max_num ignores either NaN, including signaling NaNs; host fmax
        # can instead quiet and propagate a signaling NaN.
        maximum = np.where(np.isnan(af), bf, np.where(np.isnan(bf), af, np.fmax(af, bf)))
        both_zero = (af == 0) & (bf == 0)
        maximum[both_zero] = ((a[both_zero] & b[both_zero]) & np.uint64(1 << 63)).view(np.float64)
        expected = [af / bf, maximum, af.astype(np.float32), c.view(np.float32).astype(np.float64)]
    for name, actual, reference in zip(['division', 'maximum', 'narrow', 'wide'], outputs, expected):
        actual = np.asarray(actual)
        equal = (actual == reference.view(actual.dtype)) | (np.isnan(actual.view(reference.dtype)) & np.isnan(reference))
        assert equal.all(), (name, np.flatnonzero(~equal)[:8], actual[~equal][:8], reference[~equal][:8])
    report = {'pass': True, 'cases_per_operation': 8192,
              'operations': ['binary64 RNE division', 'binary64 max', 'RNE binary64 to binary32', 'binary32 to binary64'],
              'oracle': 'independent host IEEE binary64 operations; NaN payloads excluded',
              'ops_sha256': hashlib.sha256(header.encode()).hexdigest()}
    output.mkdir(parents=True, exist_ok=True)
    (output / 'binary64.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


def guide_fixture(width, height):
    y, x = np.mgrid[:height, :width].astype(np.float32)
    color = np.stack([0.1 + x / width, 0.15 + y / height,
                      0.2 + ((x.astype(int) ^ y.astype(int)) & 3) * 0.02, np.ones_like(x)], axis=-1).astype(np.float16)
    normals = np.zeros((height, width, 4), np.float16)
    normals[..., 2:] = 1
    return {'Color': color, 'MotionVectors': np.zeros((height, width, 2), np.float16),
              'Depth': np.full((height, width, 1), 0.5, np.float16), 'GBuffer.Normals': normals,
              'GBuffer.Roughness': np.full((height, width, 1), 0.4, np.float16),
              'DLSS.Input.DiffuseAlbedo': np.full((height, width, 4), 0.6, np.float16),
              'DLSS.Input.SpecularAlbedo': np.full((height, width, 4), 0.04, np.float16)}


def reconstruction(output, selectors, width, height, frames, backend='oracle', reference=None):
    from rr_mlx.model import Model

    inputs = guide_fixture(width, height)
    output.mkdir(parents=True, exist_ok=True)
    results = []
    for selector in selectors:
        model = Model(selector, inputs, backend=backend)
        for frame in range(frames):
            directory = output / f'model{selector}-{width}x{height}-frame{frame}'
            image, receipts = model.evaluate(inputs, reset=frame == 0, capture_directory=directory)
            np.save(directory / 'output.npy', image)
            if reference is not None:
                compare_reference_capture(receipts, directory, reference / directory.name)
            assert np.ptp(image[..., :3].astype(np.float32)) > 0, 'Constant reconstructed RGB output'
            reduction_checks = []
            for index, receipt in enumerate(receipts):
                if receipt['module'] != 'module-08.ptx':
                    continue
                params = (directory / f'{index:02d}-params.bin').read_bytes()
                count, source_pointer, destination_pointer = struct.unpack('<I4xQQ', params)
                source_span, destination_span = model.spans[source_pointer >> 32], model.spans[destination_pointer >> 32]
                assert source_span['virtual_base'] == source_pointer and destination_span['virtual_base'] == destination_pointer
                previous = np.load(directory / f'{index - 1:02d}-{receipts[index - 1]["entry"]}.npy')
                current = np.load(directory / f'{index:02d}-{receipt["entry"]}.npy')
                start = source_span['arena_offset']
                values = previous[start:start + count * 4].copy().view(np.float32)
                blocks = receipt['cuda_grid'][0]
                expected = np.bincount(np.arange(count) // 256 % blocks, weights=values, minlength=blocks)
                start = destination_span['arena_offset']
                actual = current[start:start + blocks * 4].copy().view(np.float32)
                np.testing.assert_allclose(actual, expected, rtol=2e-6, atol=1e-12)
                reduction_checks.append({'blocks': blocks, 'elements': count,
                                         'max_absolute_error': float(np.max(np.abs(actual - expected)))})
            result = {'selector': selector, 'frame': frame, 'reset': frame == 0,
                      'shape': image.shape, 'output_sha256': hashlib.sha256(image.tobytes()).hexdigest(),
                      'rgb_min': float(image[..., :3].min()), 'rgb_max': float(image[..., :3].max()),
                      'next_parity': model.parity, 'next_cycle': model.cycle, 'dispatches': receipts,
                      'packed_weight_sha256': model.weight_hashes, 'auxiliary_sum_checks': reduction_checks, 'pass': True}
            (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
            results.append(result)
    report = {'pass': True, 'frames': results, 'nvidia_parity': 'not available; not claimed'}
    (output / 'execution-check.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'pass': True, 'models': selectors, 'frames_each': frames, 'size': [width, height]}))


def state_edges(output, backend='oracle', reference=None):
    from rr_mlx.model import Model

    inputs = guide_fixture(32, 32)
    model = Model(5, inputs, backend=backend)
    baseline = None
    results = []
    for frame in range(9):
        guides, metadata = inputs, {}
        if frame == 1:
            guides = {name: data.copy() for name, data in inputs.items()}
            guides['Color'] = np.roll(guides['Color'], 1, axis=1)
            guides['MotionVectors'][...] = (0.5, -0.25)
            camera = np.eye(4, dtype=np.float32)
            camera[3, 0] = 0.01
            metadata = {'jitter': (0.25, -0.25), 'pre_exposure': 1.25,
                        'world_to_view': camera.reshape(-1).tolist()}
        directory = output / f'frame{frame}'
        image, receipts = model.evaluate(guides, reset=frame in (0, 8), metadata=metadata,
                                         capture_directory=directory)
        assert np.ptp(image[..., :3].astype(np.float32)) > 0
        assert (model.parity, model.cycle) == ((frame + 1) % 2, (frame + 1) % 8)
        np.save(directory / 'output.npy', image)
        if reference is not None:
            compare_reference_capture(receipts, directory, reference / directory.name)
        if frame == 0:
            baseline = image.copy()
        elif frame == 8:
            np.testing.assert_array_equal(image, baseline)
        results.append({'frame': frame, 'reset': frame in (0, 8), 'metadata': metadata,
                        'output_sha256': hashlib.sha256(image.tobytes()).hexdigest(),
                        'dispatches': len(receipts), 'next_cycle': model.cycle})
    state = (model.parity, model.cycle, np.asarray(model.arena).copy())
    invalid = [dict(inputs), dict(inputs), dict(inputs), dict(inputs)]
    del invalid[0]['Depth']
    invalid[1]['Depth'] = inputs['Depth'][:-1]
    invalid[2]['Depth'] = 'not an array'
    invalid[3]['Depth'] = np.full_like(inputs['Depth'], np.nan)
    for guides in invalid:
        try:
            model.evaluate(guides)
        except ValueError:
            pass
        else:
            raise AssertionError('Invalid guides accepted')
        assert (model.parity, model.cycle) == state[:2]
        np.testing.assert_array_equal(np.asarray(model.arena), state[2])
    report = {'pass': True, 'frames': results, 'reset_at_phase_zero_exact': True,
              'invalid_guides_rejected_without_state_change': len(invalid),
              'nvidia_parity': 'not available; not claimed'}
    (output / 'state-edges.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'pass': True, 'scenario': 'state-edges', 'frames': len(results)}))


def geometry_edges(output, selectors, backend='oracle', reference=None):
    from rr_mlx.model import Model

    inputs = guide_fixture(64, 64)
    inputs.update({'ExposureTexture': np.ones((1, 1, 1), np.float32),
                   'DLSSD.Alpha': np.full((64, 64, 1), 0.75, np.float16),
                   'DLSS.TransparencyLayer': np.full((64, 64, 4), 0.1, np.float16),
                   'DLSS.TransparencyLayerOpacity': np.full((64, 64, 1), 0.25, np.float16),
                   'DLSS.TransparencyLayerMvecs': np.full((64, 64, 2), 0.125, np.float16)})
    atlas = {name: (data if name == 'ExposureTexture' else
                    np.pad(data, ((1, 2), (2, 3), (0, 0)), constant_values=7)) for name, data in inputs.items()}
    origins = {name: (2, 1) for name in inputs if name != 'ExposureTexture'}
    results = []
    for selector in selectors:
        full_image = None
        for label, guides, kwargs in [('full', inputs, {}),
                                      ('subrect', atlas, {'input_size': (64, 64), 'input_origins': origins})]:
            # 3:2 is an original direct-output special case. Use 25:16 to
            # exercise the separate downsample rather than assert it exists.
            model = Model(selector, guides, output_size=(100, 100), multiscale=True,
                          alpha_upscaling=True, backend=backend, **kwargs)
            assert model.host.geometry['separate_downsample']
            directory = output / f'model{selector}-{label}'
            image, receipts = model.evaluate(guides, reset=True, capture_directory=directory)
            assert image.shape == (100, 100, 4) and np.ptp(image[..., :3].astype(np.float32)) > 0
            assert any(r['module'] == 'module-56.ptx' for r in receipts)
            alpha = next(span for span in model.spans.values() if span.get('name') == 'OutputAlpha')
            start = alpha['arena_offset']
            np.testing.assert_array_equal(
                np.asarray(model.arena)[start:start + alpha['bytes']].view(np.float16), np.float16(0.75))
            np.save(directory / 'output.npy', image)
            if reference is not None:
                compare_reference_capture(receipts, directory, reference / directory.name)
            if full_image is None:
                full_image = image
            else:
                np.testing.assert_array_equal(image, full_image)
            results.append({'selector': selector, 'layout': label, 'shape': image.shape,
                            'output_sha256': hashlib.sha256(image.tobytes()).hexdigest(),
                            'dispatches': len(receipts)})
    (output / 'geometry-edges.json').write_text(json.dumps({'pass': True, 'cases': results}, indent=2) + '\n')
    print(json.dumps({'pass': True, 'scenario': 'geometry-edges', 'cases': len(results)}))



def tensor_primitives(output):
    from rr_mlx import ptx, runtime, lifted

    source = '''.version 8.7
.target sm_89
.address_size 64
.visible .entry tensor_check(.param .align 8 .b8 argument[8]) {
.reg .b32 %r<16>, %a<4>, %b<2>, %c<2>, %d<4>, %e<2>;
.reg .b64 %rd<4>; .reg .pred %p;
.shared .align 4 .b8 tile[256];
ld.param.u64 %rd0, [argument];
mov.u32 %r0, %tid.x; mov.u32 %r1, %ctaid.x;
mov.u32 %r2, tile; shl.b32 %r3, %r0, 2; add.s32 %r2, %r2, %r3;
mad.lo.s32 %r4, %r1, 100, %r0; st.shared.u32 [%r2], %r4;
bar.sync 0;
mov.b32 %a0, 0x38383838; mov.b32 %a1, %a0; mov.b32 %a2, %a0; mov.b32 %a3, %a0;
mov.b32 %b0, %a0; mov.b32 %b1, %a0;
mov.b32 %c0, 0x3c003c00; mov.b32 %c1, %c0;
mov.u32 %r5, 0; shr.u32 %r6, %r0, 5; add.s32 %r6, %r6, 2;
loop:
mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16 {%d0,%d1}, {%a0,%a1,%a2,%a3}, {%b0,%b1}, {%c0,%c1};
mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16 {%d2,%d3}, {%a0,%a1,%a2,%a3}, {%b0,%b1}, {%d0,%d1};
mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16 {%e0,%e1}, {%a0,%a1,%a2,%a3}, {%b0,%b1}, {%c0,%c1};
mov.b32 %c0, %d2; mov.b32 %c1, %d3;
add.s32 %r5, %r5, 1; setp.lt.u32 %p, %r5, %r6; @%p bra loop;
bar.sync 0; ld.shared.u32 %r7, [%r2];
mad.lo.s32 %r8, %r1, 64, %r0; mul.wide.u32 %rd1, %r8, 16; add.s64 %rd2, %rd0, %rd1;
st.global.v4.u32 [%rd2], {%d2,%d3,%e0,%r7}; ret;
}'''
    spec = {'schema': 'rr.mlx.dispatch.v1', 'entry': 'tensor_check', 'module': 'independent-tensor-fixture',
            'model_selector': 5, 'params': struct.pack('<Q', 4096),
            'cuda_grid': [2, 1, 1], 'cuda_block': [64, 1, 1], 'global_symbols': {},
            'resources': [{'resource_id': 1, 'handle': 4096, 'virtual_base': 4096,
                           'arena_offset': 64, 'bytes': 2048, 'buffer': 'arena',
                           'access': 'readwrite', 'view': {'kind': 'global'}}],
            'state_before': {}, 'source_locations': []}
    original = np.full(2176, 0xa5, np.uint8)
    arena = mx.array(original)
    oracle, _ = runtime.execute(spec, ptx.compile_kernel(source, 'tensor_check', spec), arena, b'', b'')
    compiled = lifted.compile_lifted(source, 'tensor_check', spec)
    actual, receipt = lifted.execute_lifted(spec, compiled, arena, b'', b'')
    np.testing.assert_array_equal(np.asarray(actual), np.asarray(oracle))
    np.testing.assert_array_equal(np.asarray(arena), original)
    expected = original.copy()
    rows = expected[64:-64].view(np.uint32).reshape(2, 64, 4)
    for warp, iterations in ((0, 2), (1, 3)):
        d = int(np.float16(1 + iterations * 64).view(np.uint16))
        e = int(np.float16(1 + (iterations - 1) * 64 + 32).view(np.uint16))
        rows[:, warp*32:(warp+1)*32, :2] = d | (d << 16)
        rows[:, warp*32:(warp+1)*32, 2] = e | (e << 16)
    rows[:, :, 3] = np.arange(64, dtype=np.uint32) + np.array([0, 100], np.uint32)[:, None]
    np.testing.assert_array_equal(np.asarray(actual), expected)
    assert receipt['oracle_neural_mma_count'] == 0 and receipt['dynamic_lifted_mma_count'] == 30
    faults = []
    for name, bit, text, dispatch in [
        ('global OOB', 1, source, {**spec, 'params': struct.pack('<Q', 8192)}),
        ('shared before write', 128, source.replace('st.shared.u32 [%r2], %r4;', ''), spec),
        ('trap', 16, source.replace('ret;', 'trap; ret;'), spec),
    ]:
        try:
            lifted.execute_lifted(dispatch, lifted.compile_lifted(text, 'tensor_check', dispatch), arena, b'', b'')
        except runtime.DispatchError as error:
            assert error.diagnostics['raw'][0] == bit, error.diagnostics
            np.testing.assert_array_equal(np.asarray(arena), original)
            faults.append({'case': name, 'raw': error.diagnostics['raw']})
        else:
            raise AssertionError(f'{name}: optimized execution failed to reject GPU fault')

    # Random finite FP8 words and half accumulators exercise every lane/fragment.
    random_source = '''.version 8.7
.target sm_89
.address_size 64
.visible .entry tensor_check(.param .align 8 .b8 argument[8]) {
.reg .b32 %r<4>, %a<4>, %b<2>, %c<2>, %d<2>; .reg .b64 %rd<5>;
ld.param.u64 %rd0, [argument]; mov.u32 %r0, %tid.x; mov.u32 %r1, %ctaid.x;
mul.wide.u32 %rd1, %r1, 1280; add.s64 %rd0, %rd0, %rd1;
mul.wide.u32 %rd1, %r0, 16; add.s64 %rd2, %rd0, %rd1;
ld.global.v4.u32 {%a0,%a1,%a2,%a3}, [%rd2];
mul.wide.u32 %rd1, %r0, 8; add.s64 %rd2, %rd0, %rd1;
ld.global.v2.u32 {%b0,%b1}, [%rd2+512]; ld.global.v2.u32 {%c0,%c1}, [%rd2+768];
mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16 {%d0,%d1}, {%a0,%a1,%a2,%a3}, {%b0,%b1}, {%c0,%c1};
st.global.v2.u32 [%rd2+1024], {%d0,%d1}; ret;
}'''
    rng = np.random.default_rng(20261011)
    random_arena = np.full(128*1280+128, 0xa5, np.uint8)
    blocks = random_arena[64:-64].reshape(128, 1280)
    blocks[:, :768] = rng.integers(0, 256, (128, 768), dtype=np.uint8)
    blocks[:, :768][(blocks[:, :768] & 127) == 127] = 0
    c_bits = rng.integers(0, 65536, (128, 128), dtype=np.uint16)
    c_bits[(c_bits & 0x7c00) == 0x7c00] = 0
    blocks[:, 768:1024] = c_bits.view(np.uint8)
    blocks[0, :512] = 0; blocks[1, :512] = 128
    blocks[:2, 512:768] = 0x38
    blocks[:2, 768:1024] = np.full((2, 128), 0x8000, np.uint16).view(np.uint8)
    random_spec = {**spec, 'cuda_grid': [128, 1, 1], 'cuda_block': [32, 1, 1],
                   'resources': [{**spec['resources'][0], 'bytes': 128*1280}]}
    input_array = mx.array(random_arena)
    oracle, _ = runtime.execute(random_spec, ptx.compile_kernel(random_source, 'tensor_check', random_spec), input_array, b'', b'')
    actual, random_receipt = lifted.execute_lifted(random_spec, lifted.compile_lifted(random_source, 'tensor_check', random_spec), input_array, b'', b'')
    np.testing.assert_array_equal(np.asarray(actual), np.asarray(oracle))
    np.testing.assert_array_equal(np.asarray(input_array), random_arena)
    report = {'pass': True, 'source_loop_two_warps_two_ctas': receipt,
              'random_fp8_half_outputs': 16384, 'random_fragments': random_receipt,
              'diagnostics': faults, 'immutable_input_and_whole_arena_exact': True}
    output.mkdir(parents=True, exist_ok=True)
    (output / 'tensor.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'pass': True, 'scenario': 'tensor', 'random_outputs': 16384}))


def compare_reference_capture(receipts, directory, reference):
    for number, receipt in enumerate(receipts):
        previous = json.loads((reference / f'{number:02d}-receipt.json').read_text())
        for key in ('entry', 'parameter_sha256', 'executed_ptx_sha256'):
            assert receipt[key] == previous[key], (number, key)
        filename = f'{number:02d}-{receipt["entry"]}.npy'
        np.testing.assert_array_equal(np.load(directory / filename), np.load(reference / filename))
    neural = receipts[4:15]
    assert len(neural) == 11 and all(r['backend'] in ('mlx_tensor', 'ptx_oracle') for r in neural)
    assert any(r['backend'] == 'mlx_tensor' for r in neural)
    assert all(r['backend'] == 'ptx_oracle' or
               (r['oracle_neural_mma_count'] == 0 and r['dynamic_lifted_mma_count'] > 0)
               for r in neural)


def optimized(output, selectors):
    from rr_mlx import runtime, lifted
    from rr_mlx.model import Model, TENSOR_STAGE_KERNELS

    reference_root = ROOT / 'build/local/dlss-rr-mlx-20261011/pooling-fixed'
    output.mkdir(parents=True, exist_ok=True)
    frames, timings = [], []
    for selector in selectors:
        inputs = guide_fixture(32, 32)
        model = Model(selector, inputs, backend='optimized')
        first_receipts = None
        for frame in range(1):
            name = f'model{selector}-32x32-frame{frame}'
            directory, reference = output / name, reference_root / name
            started = perf_counter()
            image, receipts = model.evaluate(inputs, reset=frame == 0, capture_directory=directory)
            elapsed = perf_counter() - started
            np.save(directory / 'output.npy', image)
            compare_reference_capture(receipts, directory, reference)
            np.testing.assert_array_equal(image, np.load(reference / 'output.npy'))
            frames.append({'selector': selector, 'frame': frame, 'all_16_arenas_exact': True,
                           'neural_stages': 11, 'frame_with_capture_seconds': elapsed,
                           'neural_dispatch_seconds': sum(r['gpu_execution_seconds'] for r in receipts[4:15]),
                           'next_parity': model.parity, 'next_cycle': model.cycle})
            assert (model.parity, model.cycle) == ((frame + 1) % 2, (frame + 1) % 8)
            if frame == 0:
                first_receipts = receipts
        directory = output / f'model{selector}-32x32-frame0'
        capture = json.loads((directory / 'host-capture.json').read_text())['capture']
        stages = []
        for number in range(4, 15):
            spec = model.dispatch_spec(capture['dispatches'][number], capture)
            key = ('optimized', spec['entry'], tuple(spec['cuda_grid']),
                   tuple(spec['cuda_block']), tuple(v['handle'] for v in spec['resources']))
            compiled = model.compiled[key]
            previous = np.load(directory / f'{number-1:02d}-{first_receipts[number-1]["entry"]}.npy')
            expected = np.load(directory / f'{number:02d}-{spec["entry"]}.npy')
            arena = mx.array(previous)
            mx.eval(arena, model.weights, model.constants)
            tensor_stage = capture['dispatches'][number]['kernel'] in TENSOR_STAGE_KERNELS
            oracle_compiled = compiled['oracle'] if tensor_stage else compiled
            # Both backends build a fresh evaluated operation on this immutable
            # input for every sample. No feedback or cached output timing.
            runtime.execute(spec, oracle_compiled, arena, model.weights, model.constants)
            if tensor_stage:
                lifted.execute_lifted(spec, compiled, arena, model.weights, model.constants)
            samples = {'oracle': [], 'selected': []}
            for repeat in range(5):
                for backend in (('oracle', 'selected') if repeat % 2 == 0 else ('selected', 'oracle')):
                    mx.synchronize()
                    started = perf_counter()
                    if backend == 'oracle' or not tensor_stage:
                        result, _ = runtime.execute(spec, oracle_compiled, arena, model.weights, model.constants)
                    else:
                        result, _ = lifted.execute_lifted(spec, compiled, arena, model.weights, model.constants)
                    mx.synchronize()
                    samples[backend].append(perf_counter() - started)
                    np.testing.assert_array_equal(np.asarray(result), expected)
            stage = {'entry': spec['entry'], 'samples_seconds': samples,
                     'median_seconds': {k: float(np.median(v)) for k, v in samples.items()}}
            stages.append(stage)
            (output / f'model{selector}-timing-progress.json').write_text(json.dumps(stages, indent=2) + '\n')
            print(json.dumps({'selector': selector, **stage}), flush=True)
        totals = {k: sum(s['median_seconds'][k] for s in stages) for k in ('oracle', 'selected')}
        timings.append({'selector': selector, 'stages': stages, 'summed_neural_medians_seconds': totals,
                        'speedup': totals['oracle'] / totals['selected'],
                        'tensor_stage_kernels': sorted(TENSOR_STAGE_KERNELS)})
    report = {'pass': all(t['speedup'] > 1 for t in timings), 'frames': frames, 'timings': timings,
              'scope': 'Strict whole-arena equivalence and evaluated dispatch wall latency; not NVIDIA parity or game FPS.'}
    (output / 'optimized.json').write_text(json.dumps(report, indent=2) + '\n')
    assert report['pass'], 'High-level execution has not improved actual neural latency'
    print(json.dumps({'pass': True, 'scenario': 'optimized', 'speedups': [t['speedup'] for t in timings]}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--scenario', choices=['primitives', 'instruction-edges', 'binary64', 'compiler', 'views', 'reconstruction', 'state-edges', 'geometry-edges', 'tensor', 'optimized'], default='reconstruction')
    parser.add_argument('--backend', choices=['oracle', 'optimized'], default='oracle')
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--selectors', type=int, nargs='+', choices=[4, 5], default=[4, 5])
    parser.add_argument('--width', type=int, default=32)
    parser.add_argument('--height', type=int, default=32)
    parser.add_argument('--frames', type=int, default=2)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/local/dlss-rr-mlx-20261011')
    args = parser.parse_args()
    if args.scenario == 'primitives':
        primitives(args.output)
    elif args.scenario == 'instruction-edges':
        instruction_edges(args.output)
    elif args.scenario == 'binary64':
        binary64(args.output)
    elif args.scenario == 'compiler':
        compiler(args.output)
    elif args.scenario == 'views':
        views(args.output)
    elif args.scenario == 'state-edges':
        state_edges(args.output, args.backend, args.reference)
    elif args.scenario == 'geometry-edges':
        geometry_edges(args.output, args.selectors, args.backend, args.reference)
    elif args.scenario == 'tensor':
        tensor_primitives(args.output)
    elif args.scenario == 'optimized':
        optimized(args.output, args.selectors)
    else:
        reconstruction(args.output, args.selectors, args.width, args.height, args.frames, args.backend, args.reference)
