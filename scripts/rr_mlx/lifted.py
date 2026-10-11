"""Source-PC neural SSA and exact MLX tensors over one original CTA checkpoint."""
from collections import defaultdict
from functools import lru_cache
from hashlib import sha256
from math import prod
from pathlib import Path
from time import perf_counter

import mlx.core as mx
import numpy as np

from . import ptx, runtime

ABI = 'rr.mlx.tensor.v1/ordered-k32/half-publication/serial-cta'
MMA = 'mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16'


@mx.compile
def ordered_contract(a, b, c):
    # E4M3 products are exact FP32; ascending additions equal ordered FMA.
    result = c.astype(mx.float32)
    for k in range(32):
        result = result + a[..., :, k:k + 1] * b[..., k:k + 1, :]
    return result.astype(mx.float16)


@lru_cache(maxsize=1)
def _fp8_table():
    bits = np.empty(256, dtype=np.uint16)
    for code in range(256):
        magnitude, sign = code & 127, (code & 128) << 8
        if magnitude == 127:
            bits[code] = 0x7e00
        elif magnitude < 8:
            bits[code] = sign | np.float16(magnitude * 2**-9).view(np.uint16)
        else:
            bits[code] = sign | (((magnitude >> 3) + 8) << 10) | ((magnitude & 7) << 7)
    return mx.array(bits.view(np.float16).astype(np.float32))


def phase_metadata(parser):
    """Version reads before writes; immutable versions eliminate WAR/WAW races."""
    slots = {name: index for index, name in enumerate(parser.registers)}
    runs = []
    for pc, inst in enumerate(parser.instructions):
        if inst.opcode.startswith('mma.'):
            if inst.opcode != MMA:
                raise ptx.CompileError(f'Unsupported neural MMA {inst.opcode}')
            if not runs or runs[-1][-1][0] != pc - 1:
                runs.append([])
            runs[-1].append((pc, inst))
    phases = []
    targets = set(parser.labels.values())
    for run in runs:
        start, end = run[0][0], run[-1][0] + 1
        if any(start < target < end for target in targets):
            raise ptx.CompileError('Interior branch target in neural phase')
        versions, producers, levels = defaultdict(int), {}, {}
        nodes, waves, live_in = [], defaultdict(list), set()
        for pc, inst in run:
            fragments = [tuple(slots[name] for name in ptx._split(arg[1:-1])) for arg in inst.args]
            if tuple(map(len, fragments)) != (2, 4, 2, 2):
                raise ptx.CompileError(f'Invalid MMA fragments at PC {pc}')
            d, a, b, c = fragments
            # These source entries have no predicated MMA. A different opcode
            # contract must not silently bypass convergence validation.
            if inst.predicate is not None:
                raise ptx.CompileError(f'Predicated neural phase at PC {pc}')
            reads = {kind: tuple((slot, versions[slot]) for slot in fragment)
                     for kind, fragment in (('a', a), ('b', b), ('c', c))}
            dependencies = sorted({producers[ref] for refs in reads.values() for ref in refs if ref in producers})
            level = 1 + max((levels[index] for index in dependencies), default=-1)
            live_in.update(ref for refs in reads.values() for ref in refs if ref[1] == 0)
            writes = []
            for slot in d:
                versions[slot] += 1
                ref = (slot, versions[slot])
                producers[ref] = len(nodes)
                writes.append(ref)
            node = {'id': len(nodes), 'pc': pc, 'line': inst.line, 'opcode': inst.opcode,
                    'predicate': inst.predicate, 'reads': reads, 'writes': tuple(writes),
                    'dependencies': dependencies, 'wave': level}
            levels[len(nodes)] = level
            waves[level].append(len(nodes))
            nodes.append(node)
        phases.append({'id': len(phases) + 1, 'start_pc': start, 'end_pc': end,
                       'source_lines': [run[0][1].line, run[-1][1].end_line],
                       'nodes': nodes, 'waves': [waves[level] for level in sorted(waves)],
                       'live_in': sorted(live_in),
                       'live_out': sorted((slot, version) for slot, version in versions.items() if version),
                       'chain_rounding': 'RNE half publication after every original K32 MMA',
                       'uniformity': 'original GPU full-warp scheduler; no interior branch targets'})
    return slots, phases


def compile_lifted(source, entry, dispatch_spec):
    """Compile executed repaired PTX, never the raw extraction phase PC table."""
    started = perf_counter()
    parser = ptx.Parser(source, entry)
    slots, phases = phase_metadata(parser)
    if not phases:
        raise ptx.CompileError(f'{entry} is not a neural MMA entry')
    if prod(dispatch_spec['cuda_block']) % 32:
        raise ptx.CompileError('Neural CTA must contain complete original warps')
    cuts = tuple((phase['start_pc'], phase['end_pc']) for phase in phases)
    resume = ptx.compile_resume(source, entry, dispatch_spec, cuts)
    if resume['register_slots'] != slots:
        raise ptx.CompileError('Scalar/tensor checkpoint register slots differ')
    oracle = ptx.compile_kernel(source, entry, dispatch_spec)
    cfg = []
    for pc, inst in enumerate(parser.instructions):
        base = inst.opcode.split('.')[0]
        successors = []
        if base == 'bra':
            successors.append(parser.labels[inst.args[0]])
        if (base not in ('bra', 'ret', 'trap', 'brkpt') or inst.predicate) and pc + 1 < len(parser.instructions):
            successors.append(pc + 1)
        cfg.append(sorted(set(successors)))
    mma_count = sum(len(phase['nodes']) for phase in phases)
    assert mma_count == sum(inst.opcode == MMA for inst in parser.instructions)
    return {'schema': 'rr.mlx.lifted-entry.v1', 'entry': entry,
            'executed_ptx_sha256': sha256(source.encode()).hexdigest(),
            'oracle': oracle, 'resume': resume, 'register_slots': slots,
            'phase_table': phases, 'cfg': cfg,
            'coverage': {'phases': len(phases), 'lifted_mma': mma_count,
                         'original_mma': mma_count, 'unlifted_mma': 0,
                         'waves': sum(len(phase['waves']) for phase in phases)},
            'cuda_block': tuple(dispatch_spec['cuda_block']),
            'cuda_grid': tuple(dispatch_spec['cuda_grid']),
            'compile_seconds': perf_counter() - started}


def gather_fragments(checkpoint, phase, wave_index):
    """Gather independent SSA nodes x selected original warps, entirely in MLX."""
    values = checkpoint['values']
    warps = checkpoint['warps']
    warp_indices = mx.array(warps, dtype=mx.int32)
    batches = []
    for index in phase['waves'][wave_index]:
        node = phase['nodes'][index]
        fragments = []
        for kind in ('a', 'b', 'c'):
            fragments.append(mx.stack([values[ref].reshape(-1, 32)[warp_indices] for ref in node['reads'][kind]], axis=-1).astype(mx.uint32))
        a, b, c = fragments
        a_bytes = ((a[..., None] >> mx.array([0, 8, 16, 24], dtype=mx.uint32)) & 255)
        b_bytes = ((b[..., None] >> mx.array([0, 8, 16, 24], dtype=mx.uint32)) & 255)
        batch = len(warps)
        aa = mx.take(_fp8_table(), a_bytes).reshape(batch, 8, 4, 2, 2, 4).transpose(0, 4, 1, 3, 2, 5).reshape(batch, 16, 32)
        bb = mx.take(_fp8_table(), b_bytes).reshape(batch, 8, 4, 2, 4).transpose(0, 3, 2, 4, 1).reshape(batch, 32, 8)
        c_bits = ((c[..., None] >> mx.array([0, 16], dtype=mx.uint32)) & 65535).astype(mx.uint16)
        cc = c_bits.view(mx.float16).reshape(batch, 8, 4, 2, 2).transpose(0, 3, 1, 2, 4).reshape(batch, 16, 8)
        batches.append((aa, bb, cc))
    return (*[mx.concatenate([batch[index] for batch in batches], axis=0) for index in range(3)],
            mx.ones((len(batches) * len(warps),), dtype=mx.bool_))


def scatter_fragments(checkpoint, phase, wave_index, result, valid):
    """Publish each original half result to fresh SSA versions, not shared slots."""
    count = len(checkpoint['warps'])
    bits = result.view(mx.uint16)
    # rr_half_bits canonicalizes all NaNs, independent of input sign/payload.
    bits = mx.where((bits & 0x7fff) > 0x7c00, mx.array(0x7e00, mx.uint16), bits)
    halves = bits.reshape(-1, 2, 8, 4, 2).transpose(0, 2, 3, 1, 4).reshape(-1, 32, 2, 2).astype(mx.uint32)
    words = halves[..., 0] | (halves[..., 1] << 16)
    values = checkpoint['values'].copy()
    selected = checkpoint['selected']
    # Scatter indices are unique original lane IDs. All other lanes keep raw bits.
    lanes = (mx.array(checkpoint['warps'], dtype=mx.int32)[:, None] * 32 + mx.arange(32)[None, :]).reshape(-1)
    for offset, index in enumerate(phase['waves'][wave_index]):
        node = phase['nodes'][index]
        node_valid = mx.repeat(valid[offset * count:(offset + 1) * count], 32)
        for fragment, ref in enumerate(node['writes']):
            old = checkpoint['registers'][ref[0]]
            raw = words[offset * count:(offset + 1) * count, :, fragment].reshape(-1).astype(mx.uint64)
            # MLX Metal scatter supports uint32, not uint64. Update both words
            # independently: these are raw register bits, not integer sums.
            old_words = old.view(mx.uint32).reshape(-1, 2)
            raw_words = raw.view(mx.uint32).reshape(-1, 2)
            new = old_words.at[lanes].add(mx.where(
                node_valid[:, None], raw_words - old_words[lanes], mx.zeros_like(raw_words)))
            values[ref] = mx.where(selected, new.reshape(-1).view(mx.uint64), old)
    return {**checkpoint, 'values': values}


def _tensor_phase(registers, control, phase, warps):
    selected = (control[:, 1] != 0) & (control[:, 0] == phase['start_pc'])
    checkpoint = {'registers': registers, 'values': {ref: registers[ref[0]] for ref in phase['live_in']},
                  'warps': warps, 'selected': selected}
    for wave in range(len(phase['waves'])):
        a, b, c, valid = gather_fragments(checkpoint, phase, wave)
        result = ordered_contract(a, b, c)
        checkpoint = scatter_fragments(checkpoint, phase, wave, result, valid)
    slots = mx.array([ref[0] for ref in phase['live_out']], dtype=mx.int32)
    replacements = mx.stack([checkpoint['values'][ref] for ref in phase['live_out']])
    # Unique slot indices, functional update, old input remains immutable.
    words = registers.view(mx.uint32)
    registers = words.at[slots].add(replacements.view(mx.uint32) - words[slots]).view(mx.uint64)
    pc = mx.where(selected, mx.array(phase['end_pc'], dtype=mx.uint32), control[:, 0])
    control = mx.stack((pc, control[:, 1]), axis=1)
    return registers, control


def execute_lifted(dispatch_spec, lifted_entry, arena, weights, constants):
    """Transactional dispatch: bounded scalar GPU slices and tensors, no oracle."""
    started = perf_counter()
    compiled = lifted_entry['resume']
    if dispatch_spec['entry'] != lifted_entry['entry']:
        raise ValueError('Dispatch entry differs from LiftedEntry')
    if tuple(dispatch_spec['cuda_block']) != lifted_entry['cuda_block'] or tuple(dispatch_spec['cuda_grid']) != lifted_entry['cuda_grid']:
        raise ValueError('Dispatch geometry differs from LiftedEntry')
    arena = runtime._bytes_array(arena, 'arena')
    weights = runtime._bytes_array(weights, 'weights')
    constants = runtime._bytes_array(constants, 'constants')
    params, table, launch = runtime.pack_dispatch_metadata(dispatch_spec, arena.size, weights.size, constants.size)
    if params.size != compiled['param_bytes']:
        raise ValueError('captured parameter byte count differs from CompiledEntry')
    expected_inputs = runtime.INPUT_NAMES + ('registers_in', 'shared_in', 'defined_in', 'control_in', 'resume_info')
    expected_outputs = runtime.OUTPUT_NAMES + ('registers_out', 'shared_out', 'defined_out', 'control_out', 'status')
    if tuple(compiled['input_names']) != expected_inputs or tuple(compiled['output_names']) != expected_outputs:
        raise ValueError('Scalar resume input/output ABI does not match tensor runtime')
    source_hash = sha256(compiled['source'].encode()).hexdigest()
    program_hash = sha256(compiled['program']).hexdigest()
    if source_hash != compiled['source_sha256'] or program_hash != compiled['program_sha256']:
        raise ValueError('CompiledEntry source/program hash mismatch')
    ops = Path(__file__).with_name('ops.metal').read_text()
    header = '#pragma clang fp contract(off)\n' + ops + '\n' + compiled['header']
    kernel, program = runtime._kernel(header, compiled['source'], ABI, runtime.MLX_VERSION,
                                     tuple(compiled['input_names']), tuple(compiled['output_names']), compiled['program'])
    threads = prod(dispatch_spec['cuda_block'])
    physical = (threads, 1, 1)
    slots = len(lifted_entry['register_slots'])
    shared_bytes = max(1, compiled['shared_bytes'])
    defined_words = max(1, (compiled['shared_bytes'] + 31) // 32)
    shapes = [(arena.size,), (4,), (slots, threads), (shared_bytes,), (defined_words,), (threads, 2), (4,)]
    dtypes = [mx.uint8, mx.uint32, mx.uint64, mx.uint8, mx.uint32, mx.uint32, mx.uint32]
    registers = mx.zeros((slots, threads), dtype=mx.uint64)
    shared = mx.zeros((shared_bytes,), dtype=mx.uint8)
    defined = mx.zeros((defined_words,), dtype=mx.uint32)
    control = mx.zeros((threads, 2), dtype=mx.uint32)
    current_arena = arena
    counters = {'scalar_resume_calls': 0, 'status_syncs': 0, 'tensor_phase_calls': 0,
                'tensor_wave_calls': 0, 'dynamic_lifted_mma_count': 0,
                'oracle_neural_mma_count': 0, 'selected_warp_instances': 0}
    visits = defaultdict(int)
    warp_visits = defaultdict(int)
    by_pc = {phase['start_pc']: phase for phase in lifted_entry['phase_table']}
    raw = [0, 0, 0, 0]
    receipt = {'evaluated': True, 'backend': 'mlx_tensor', 'entry': dispatch_spec['entry'],
               'module': dispatch_spec['module'], 'model_selector': dispatch_spec['model_selector'],
               'abi': ABI, 'mlx_version': runtime.MLX_VERSION,
               'source_sha256': source_hash, 'header_sha256': sha256(header.encode()).hexdigest(),
               'program_sha256': program_hash, 'program_bytes': len(compiled['program']),
               'executed_ptx_sha256': lifted_entry['executed_ptx_sha256'],
               'oracle_source_sha256': lifted_entry['oracle']['source_sha256'],
               'oracle_program_sha256': lifted_entry['oracle']['program_sha256'],
               'compile_seconds': lifted_entry['compile_seconds'],
               'program_stride': compiled['program_stride'], 'handler_count': compiled['handler_count'],
               'cuda_grid': tuple(dispatch_spec['cuda_grid']), 'cuda_block': tuple(dispatch_spec['cuda_block']),
               'physical_grid': physical, 'physical_threadgroup': physical,
               'virtual_ctas': prod(dispatch_spec['cuda_grid']), 'arena_bytes': arena.size,
               'weights_bytes': weights.size, 'constants_bytes': constants.size, 'param_bytes': params.size,
               'resource_count': len(dispatch_spec['resources']), 'register_bytes': slots * threads * 8,
               'physical_arena_bytes': arena.size, 'math_mode': 'safe',
               'fp_contract': 'off (Clang pragma); ordered mx.compile FP32 additions',
               'state_before': dispatch_spec['state_before'],
               'numerical_limits': lifted_entry['oracle']['numerical_limits'] + [compiled['numerical_limits'][0]],
               'coverage': lifted_entry['coverage']}
    for cta in range(prod(dispatch_spec['cuda_grid'])):
        initialize = 1
        while True:
            info = mx.array([initialize, cta, 0, 0, 0, 0, 0, 0], dtype=mx.uint32)
            inputs = [program, current_arena, weights, constants, params, table, launch,
                      registers, shared, defined, control, info]
            inputs = [mx.concatenate((x.reshape(-1), mx.zeros((8 - x.size,), dtype=x.dtype))) if x.size < 8 else x for x in inputs]
            current_arena, diagnostics, registers, shared, defined, control, status = kernel(
                inputs=inputs, grid=physical, threadgroup=physical,
                output_shapes=shapes, output_dtypes=dtypes, stream=mx.gpu)
            mx.eval(status, diagnostics, control)
            counters['scalar_resume_calls'] += 1
            counters['status_syncs'] += 1
            event = status.tolist()
            raw = diagnostics.tolist()
            if raw[0] or event[0] == 3:
                faults = [name for bit, name in runtime.FAULTS.items() if raw[0] & bit]
                unknown = raw[0] & ~sum(runtime.FAULTS)
                if unknown:
                    faults.append(f'unknown fault bits 0x{unknown:x}')
                receipt.update(counters, raw=raw, faults=faults, dynamic_phase_visits=dict(visits))
                raise runtime.DispatchError(receipt)
            if event[0] == 2:
                break
            if event[0] != 1 or event[1] not in by_pc:
                raise ValueError(f'Unexpected scalar resume event {event}')
            phase = by_pc[event[1]]
            lane_state = np.asarray(control)
            selected = (lane_state[:, 1] != 0) & (lane_state[:, 0] == phase['start_pc'])
            selected_warps = selected.reshape(-1, 32)
            if np.any(np.any(selected_warps, axis=1) != np.all(selected_warps, axis=1)):
                receipt.update(counters, raw=[8, phase['start_pc'], cta, 0],
                               faults=['collective mismatch'], dynamic_phase_visits=dict(visits))
                raise runtime.DispatchError(receipt)
            warps = np.flatnonzero(np.all(selected_warps, axis=1)).tolist()
            if not warps:
                raise ValueError('Scalar resume yielded no selected original warp')
            registers, control = _tensor_phase(registers, control, phase, warps)
            counters['tensor_phase_calls'] += 1
            counters['tensor_wave_calls'] += len(phase['waves'])
            counters['dynamic_lifted_mma_count'] += len(phase['nodes']) * len(warps)
            counters['selected_warp_instances'] += len(warps)
            visits[phase['id']] += 1
            warp_visits[phase['start_pc']] += len(warps)
            initialize = 0
    mx.eval(current_arena)
    receipt.update(counters, raw=raw, faults=[], dynamic_phase_visits=dict(visits),
                   phase_visits=dict(warp_visits), execution_seconds=perf_counter() - started)
    return current_arena, receipt


def self_check():
    """CPU-only fragment bijections and SSA regression; GPU QA belongs to root."""
    a = np.arange(512).reshape(32, 4, 4)
    b = np.arange(256).reshape(32, 2, 4)
    c = np.arange(128).reshape(32, 2, 2)
    aa = a.reshape(8, 4, 2, 2, 4).transpose(3, 0, 2, 1, 4).reshape(16, 32)
    bb = b.reshape(8, 4, 2, 4).transpose(2, 1, 3, 0).reshape(32, 8)
    cc = c.reshape(8, 4, 2, 2).transpose(2, 0, 1, 3).reshape(16, 8)
    for row in range(16):
        for k in range(32):
            assert aa[row, k] == a[(row % 8) * 4 + (k % 16) // 4, 2 * (k // 16) + row // 8, k % 4]
    for k in range(32):
        for column in range(8):
            assert bb[k, column] == b[column * 4 + (k % 16) // 4, k // 16, k % 4]
    for row in range(16):
        for column in range(8):
            assert cc[row, column] == c[(row % 8) * 4 + column // 2, row // 8, column % 2]
    assert np.array_equal(cc.reshape(2, 8, 4, 2).transpose(1, 2, 0, 3).reshape(32, 2, 2), c)
    source = '.visible .entry check(.param .b8 p[8]){.reg .b32 %r<16>;'
    source += ''.join(MMA + ' {%r0,%r1},{%r2,%r3,%r4,%r5},{%r6,%r7},' + fragment + ';'
                      for fragment in ('{%r8,%r9}', '{%r10,%r11}', '{%r0,%r1}')) + 'ret;}'
    _, phases = phase_metadata(ptx.Parser(source, 'check'))
    phase = phases[0]
    assert phase['waves'] == [[0, 1], [2]]
    assert phase['nodes'][2]['reads']['c'] == phase['nodes'][1]['writes']
    assert phase['nodes'][0]['reads']['c'][0][1] == 0
    assert phase['live_out'][0][1] == 3
    return {'fragment_bijections': {'a': 512, 'b': 256, 'c': 128}, 'ssa': 'pass'}
