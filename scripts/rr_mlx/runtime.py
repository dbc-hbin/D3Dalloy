"""Exact byte-buffer execution of compiler-produced RR PTX kernels.

DispatchSpec is a plain dict with schema='rr.mlx.dispatch.v1', entry, module,
model_selector (4/5), params (bytes or rank-1 mx.uint8), cuda_grid/cuda_block
(original XYZ triples), resources, global_symbols, state_before, source_locations.
ResourceView dictionaries keep shape/storage/view/origin descriptors unchanged:
shape=(width,height,depth,channels), or a dictionary with those keys (compiler
defaults missing shape keys to 1). Current texture specialization requires
depth=1 and channels<=4; no shape means global byte storage. storage is a dtype
string, {dtype: string}, or {cubin_format: original_enum}. Optional storage
pixel_stride defaults to channel_width*channels; storage.row_pitch defaults to
resource.row_pitch, then width*pixel_stride. View is {mode:0} for normalized
clamp/linear or {mode:2} for normalized clamp/point; alternatively explicit
filter, address_mode (scalar or XY list), normalized_coordinates are required.
Optional channel_read_mode='normalized' requests integer normalization. The
compiler consumes descriptors, not a runtime heuristic. buffer defaults to 'arena';
'weights' and 'constants' are immutable. arena_offset is a byte offset in buffer,
bytes includes pitch/padding, virtual_base and handle are original unsigned64
addresses/tokens (handle=0 only for an original null binding). Aliases retain the
same resource_id; descriptors may differ between views of that allocation.

resource_table is flat uint64: [handle,virtual_base,arena_offset,bytes,buffer_id]
per view, in original resource order; buffer_id=0 arena,1 weights,2 constants.
launch is uint32[11]: grid XYZ, block XYZ, arena/weights/constants/params byte
counts, resource count. Logical extents are exact; small physical input buffers
are padded to eight elements solely to retain MLX's device address space.

The compiler owns arena copy, diagnostics initialization and serial virtual CTA
execution. This runtime launches exactly one flattened physical CTA, never a
Python per-instruction/per-CTA inference loop. diagnostics is uint32[4]: fault
mask, first PTX instruction index, virtual CTA index, reserved. Fault bits:
1 global OOB,2 shared OOB,4 missing handle,8 collective mismatch,16 trap,
32 SIMD width mismatch,64 scheduler deadlock. All nonzero masks are rejected.
128 marks shared-before-write when compiler tracking is enabled.
The returned diagnostics dict is an evaluated execution receipt, not a lazy
array or a claim of equivalence to NVIDIA. Errors do not commit a new arena.
"""

from functools import lru_cache
from hashlib import sha256
from importlib.metadata import version
import json
from math import prod
from pathlib import Path

import mlx.core as mx
import numpy as np

INPUT_NAMES = ('program', 'arena_in', 'weights', 'constants', 'params', 'resource_table', 'launch')
OUTPUT_NAMES = ('arena_out', 'diagnostics')
BUFFER_IDS = {'arena': 0, 'weights': 1, 'constants': 2}
FAULTS = {1: 'global OOB', 2: 'shared OOB', 4: 'missing handle',
          8: 'collective mismatch', 16: 'trap', 32: 'SIMD width mismatch',
          64: 'scheduler deadlock', 128: 'shared before write'}
MLX_VERSION = version('mlx')
ABI = 'rr.mlx.dispatch.v1/resource5/launch11/diagnostics4/serial-cta-gpu-program'


class DispatchError(RuntimeError):
    """A completed GPU dispatch fault; receipt remains available to the caller."""

    def __init__(self, receipt):
        self.diagnostics = receipt
        super().__init__(f"{receipt['entry']}: {', '.join(receipt['faults'])}; "
                         f"PTX instruction {receipt['raw'][1]}, CTA {receipt['raw'][2]}")


def _uint(value, bits, name):
    if type(value) is not int or not 0 <= value < 1 << bits:
        raise ValueError(f'{name} must be an unsigned {bits}-bit integer')
    return value


def _geometry(value, name):
    if not isinstance(value, (tuple, list)) or len(value) != 3:
        raise ValueError(f'{name} must contain original XYZ dimensions')
    dims = tuple(_uint(v, 32, name) for v in value)
    if not all(dims):
        raise ValueError(f'{name} dimensions must be positive')
    if prod(dims) >= 1 << 32:
        raise ValueError(f'{name} product exceeds uint32 virtual-index ABI')
    return dims


def _bytes_array(value, name):
    if isinstance(value, (bytes, bytearray, memoryview)):
        # Copy byte input at the boundary; a caller's bytearray cannot change it.
        value = mx.array(list(bytes(value)), dtype=mx.uint8)
    if not isinstance(value, mx.array) or value.dtype != mx.uint8 or value.ndim != 1:
        raise ValueError(f'{name} must be a rank-1 mx.uint8 array or raw bytes')
    _uint(value.size, 32, f'{name} byte count')
    return value


def pack_dispatch_metadata(dispatch_spec, arena_bytes, weights_bytes, constants_bytes):
    """Validate exact extents and pack params/table/launch without model math.

    Returns (params:uint8, resource_table:uint64, launch:uint32). Empty resource
    lists produce an empty table; no fake resource/null binding is inserted.
    Descriptor validation and specialization belong to compile_kernel.
    """
    if dispatch_spec['schema'] != 'rr.mlx.dispatch.v1':
        raise ValueError('unsupported DispatchSpec schema')
    if dispatch_spec['model_selector'] not in (4, 5):
        raise ValueError('model_selector must be 4 or 5')
    grid = _geometry(dispatch_spec['cuda_grid'], 'cuda_grid')
    block = _geometry(dispatch_spec['cuda_block'], 'cuda_block')
    if prod(block) > 1024:
        raise ValueError('cuda_block exceeds Metal 1024-thread threadgroup limit')
    sizes = tuple(_uint(n, 32, 'buffer byte count') for n in
                  (arena_bytes, weights_bytes, constants_bytes))
    params = _bytes_array(dispatch_spec['params'], 'params')
    # Named globals are baked into compiler helpers rather than table rows.
    # Mapping fields: virtual_base, arena_offset, bytes, buffer (default arena).
    # Integer addresses must resolve to an existing resource in the compiler.
    for symbol, span in dispatch_spec.get('global_symbols', {}).items():
        if type(span) is int:
            _uint(span, 64, f'global {symbol} address')
            continue
        buffer = span.get('buffer', 'arena')
        if buffer not in BUFFER_IDS:
            raise ValueError(f'global {symbol}: invalid buffer')
        base = _uint(span['virtual_base'], 64, f'global {symbol} virtual_base')
        offset = _uint(span['arena_offset'], 64, f'global {symbol} arena_offset')
        extent = _uint(span['bytes'], 64, f'global {symbol} bytes')
        if offset + extent > sizes[BUFFER_IDS[buffer]] or base + extent > 1 << 64:
            raise ValueError(f'global {symbol}: byte span outside {buffer} or virtual address space')
    rows = []
    handles = {}
    for index, resource in enumerate(dispatch_spec['resources']):
        buffer = resource.get('buffer', 'arena')
        if buffer not in BUFFER_IDS:
            raise ValueError(f'resource {index}: invalid buffer {buffer!r}')
        buffer_id = BUFFER_IDS[buffer]
        handle = _uint(resource['handle'], 64, 'resource handle')
        base = _uint(resource['virtual_base'], 64, 'resource virtual_base')
        offset = _uint(resource['arena_offset'], 64, 'resource arena_offset')
        extent = _uint(resource['bytes'], 64, 'resource bytes')
        if offset + extent > sizes[buffer_id] or base + extent > 1 << 64:
            raise ValueError(f'resource {index}: byte span outside {buffer} or virtual address space')
        if resource['access'] not in ('read', 'write', 'readwrite'):
            raise ValueError(f'resource {index}: invalid access')
        if buffer_id and resource['access'] != 'read':
            raise ValueError(f'resource {index}: immutable {buffer} cannot be written')
        row = [handle, base, offset, extent, buffer_id]
        if handle:
            if handle in handles and handles[handle] != row:
                raise ValueError(f'resource {index}: conflicting captured handle')
            handles[handle] = row
        rows.extend(row)
    count = _uint(len(dispatch_spec['resources']), 32, 'resource count')
    return (params, mx.array(rows, dtype=mx.uint64),
            mx.array([*grid, *block, *sizes, params.size, count], dtype=mx.uint32))


@lru_cache(maxsize=32)
def _kernel(header, source, abi, mlx_version, input_names, output_names, program):
    # Key retains exact header (including ops), source, ABI and MLX version.
    # safe does not itself prohibit FMA: Clang's contract(off) is also supplied.
    key = sha256(json.dumps([header, source, abi, mlx_version, input_names,
                             output_names], separators=(',', ':')).encode()).hexdigest()
    kernel = mx.fast.metal_kernel(
        name=f'rr_{key}', input_names=input_names, output_names=output_names,
        source=source, header=header, ensure_row_contiguous=True,
        compile_options={'math_mode': 'safe'})
    return kernel, mx.array(np.frombuffer(program, dtype='<u8'))


def execute(dispatch_spec, compiled_entry, arena, weights, constants):
    """Execute and evaluate one original dispatch, retaining immutable inputs.

    compiled_entry adds immutable GPU instruction records to the six byte-buffer inputs. param_bytes must
    match exactly; source_sha256 hashes source UTF-8 alone. header is the
    emitter-generated header; ops.metal is prepended here. The receipt records
    source/header/kernel identities, original/physical geometry and raw faults.
    No timing measurement or NVIDIA-equivalence claim is inferred from mx.eval.
    """
    arena = _bytes_array(arena, 'arena')
    weights = _bytes_array(weights, 'weights')
    constants = _bytes_array(constants, 'constants')
    params, table, launch = pack_dispatch_metadata(
        dispatch_spec, arena.size, weights.size, constants.size)
    input_names = tuple(compiled_entry['input_names'])
    output_names = tuple(compiled_entry['output_names'])
    if input_names != INPUT_NAMES or output_names != OUTPUT_NAMES:
        raise ValueError('CompiledEntry input/output ABI does not match runtime')
    if params.size != compiled_entry['param_bytes']:
        raise ValueError('captured parameter byte count differs from CompiledEntry')
    source = compiled_entry['source']
    source_hash = sha256(source.encode()).hexdigest()
    if source_hash != compiled_entry['source_sha256']:
        raise ValueError('CompiledEntry source_sha256 mismatch')
    ops = Path(__file__).with_name('ops.metal').read_text()
    header = '#pragma clang fp contract(off)\n' + ops + '\n' + compiled_entry['header']
    program = compiled_entry['program']
    program_hash = sha256(program).hexdigest()
    if program_hash != compiled_entry['program_sha256']:
        raise ValueError('CompiledEntry program_sha256 mismatch')
    kernel, gpu_program = _kernel(header, source, ABI, MLX_VERSION, input_names, output_names, program)
    physical = (prod(dispatch_spec['cuda_block']), 1, 1)
    # Retain all old inputs through mx.eval; only the compiler-created output
    # allocation is writable. Never use MLX array assignment/donation here.
    # MLX uses constant address space for inputs with fewer than eight elements.
    # Keep the compiler ABI device-addressed; launch retains exact logical sizes
    # so physical padding never makes an out-of-range PTX access valid.
    inputs = [mx.concatenate([x, mx.zeros((8 - x.size,), dtype=x.dtype)]) if x.size < 8 else x
              for x in (gpu_program, arena, weights, constants, params, table, launch)]
    register_bytes = _uint(compiled_entry['register_bytes'], 32, 'register scratch bytes')
    physical_arena_bytes = _uint(((arena.size + 7) & ~7) + register_bytes, 32, 'physical arena bytes')
    physical_arena, raw_diagnostics = kernel(
        inputs=inputs, grid=physical, threadgroup=physical,
        output_shapes=[(physical_arena_bytes,), (4,)], output_dtypes=[mx.uint8, mx.uint32],
        stream=mx.gpu)
    new_arena = physical_arena[:arena.size]
    mx.eval(new_arena, raw_diagnostics)
    raw = raw_diagnostics.tolist()
    faults = [text for bit, text in FAULTS.items() if raw[0] & bit]
    unknown = raw[0] & ~sum(FAULTS)
    if unknown:
        faults.append(f'unknown fault bits 0x{unknown:x}')
    receipt = {
        'evaluated': True, 'entry': dispatch_spec['entry'],
        'module': dispatch_spec['module'], 'model_selector': dispatch_spec['model_selector'],
        'raw': raw, 'faults': faults, 'abi': ABI, 'mlx_version': MLX_VERSION,
        'source_sha256': source_hash, 'header_sha256': sha256(header.encode()).hexdigest(),
        'program_sha256': program_hash, 'program_bytes': len(program),
        'program_stride': compiled_entry['program_stride'], 'handler_count': compiled_entry['handler_count'],
        'cuda_grid': tuple(dispatch_spec['cuda_grid']),
        'cuda_block': tuple(dispatch_spec['cuda_block']),
        'physical_grid': physical, 'physical_threadgroup': physical,
        'virtual_ctas': prod(dispatch_spec['cuda_grid']),
        'arena_bytes': arena.size, 'weights_bytes': weights.size,
        'register_bytes': register_bytes, 'physical_arena_bytes': physical_arena_bytes,
        'constants_bytes': constants.size, 'param_bytes': params.size,
        'resource_count': len(dispatch_spec['resources']),
        'math_mode': 'safe', 'fp_contract': 'off (Clang pragma)',
        'state_before': dispatch_spec['state_before'],
        'numerical_limits': compiled_entry['numerical_limits'],
    }
    if raw[0]:
        raise DispatchError(receipt)
    return new_arena, receipt


def check_metadata():
    """Root's focused packing/rejection check; no model or PTX inference."""
    spec = {
        'schema': 'rr.mlx.dispatch.v1', 'model_selector': 4,
        'params': bytes([1, 2, 3]), 'cuda_grid': (2, 3, 1),
        'cuda_block': (8, 4, 1),
        'resources': [
            {'handle': 7, 'virtual_base': 4096, 'arena_offset': 3,
             'bytes': 5, 'access': 'readwrite', 'resource_id': 'a'},
            {'handle': 8, 'virtual_base': 8192, 'arena_offset': 1,
             'bytes': 2, 'access': 'read', 'resource_id': 'w', 'buffer': 'weights'},
        ],
    }
    params, table, launch = pack_dispatch_metadata(spec, 11, 4, 0)
    assert params.tolist() == [1, 2, 3]
    assert table.tolist() == [7, 4096, 3, 5, 0, 8, 8192, 1, 2, 1]
    assert launch.tolist() == [2, 3, 1, 8, 4, 1, 11, 4, 0, 3, 2]
    for resource in (dict(spec['resources'][0], bytes=9),
                     dict(spec['resources'][1], access='write')):
        invalid = dict(spec, resources=[resource])
        try:
            pack_dispatch_metadata(invalid, 11, 4, 0)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid resource was accepted')
    return {'metadata': 'pass'}
