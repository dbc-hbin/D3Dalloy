#!/usr/bin/env python3
"""Original RR host arithmetic and resource/dispatch capture; no neural emulation."""
import hashlib
import json
from pathlib import Path
import struct
import sys
from time import perf_counter

import mlx.core as mx
import numpy as np
from . import runtime

ROOT = Path(__file__).resolve().parents[2] / 'build/local/dlss-rr-extraction-20261011'
sys.path.insert(0, str(ROOT / 'reconstruction'))
from replay.replay import engine, invoke, OBJ, STACK, STOP
from host_geometry import geometry
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import *

PACKERS = [0x72380, 0x7f670, 0x77990, 0x7b790, 0x759a0, 0x759a0,
           0x759a0, 0x7d700, 0x79880, 0x81340, 0x73e90]
DIVISORS = [1, 2, 4, 8, 16, 32, 16, 8, 4, 2, 1]
SOURCE_SHA = 'efd465933bf9a40b65f3c6d61aa079f4a4b188004e9c2b432e3375783b0029f3'


class Host:
    def __init__(self, selector, width, height, output_size=None, multiscale=False):
        self.selector, self.width, self.height = selector, width, height
        self.output_size = output_size or (width * 2, height * 2)
        self.geometry = geometry(width, height, *self.output_size, multiscale)
        iw, ih = self.geometry['internal_output']
        data = (ROOT / 'libnvidia-ngx-dlssd.so.310.7.0').read_bytes()
        assert hashlib.sha256(data).hexdigest() == SOURCE_SHA
        self.uc, _ = engine(data)
        self.state, self.network, self.packet, self.backend, self.heap = (
            0x40000000, 0x41000000, 0x42000000, 0x43000000, 0x50000000)
        for base, size in [(self.state, 0x4000), (self.network, 0x5000),
                           (self.packet, 0x1000), (self.backend, 0x4000), (self.heap, 0x8000000)]:
            self.uc.mem_map(base, size)
        self.cursor = self.heap + 0x100
        # Original main-state constructor 0x245e0 returns allocation+0x10.
        # Packed stores at 0x250ec/0x25105 initialize these four scalars.
        self.put(self.state + 0x1628, '4f', 1.0, 1.0, 0.25, 1.0)
        self.put(self.state + 0xa00, 'I', 2)
        self.put(self.state + 0x90, 'I', 3)  # Original D/E requires HDR and low-resolution motion.
        self.resources, self.views, self.bindings, self.dispatches = {}, {}, [], []
        self.phase, self.stage, self.bound_kernel = 'constructor', None, None
        self.put(OBJ + 0x10, 'Q', self.backend)
        self.put(self.backend, 'Q', self.backend + 0x1000)
        for slot in range(0, 0x300, 8):
            self.put(self.backend + 0x1000 + slot, 'Q', self.backend + 0x2000 + slot)
        self.uc.hook_add(UC_HOOK_CODE, self.hook)
        invoke(self.uc, 0x64f30, [(UC_X86_REG_RDI, self.network), (UC_X86_REG_RSI, 0),
                                (UC_X86_REG_RDX, self.backend)])
        self.put(self.state + 0x1620, 'Q', self.network)
        self.put(self.state + 0x222c, 'I', selector)
        self.put(self.state + 0x70, 'II', width, height)
        self.put(self.state + 0x88, 'II', *self.output_size)
        self.put(self.state + 0xa0c, 'BB', int(self.geometry['internal_2x']), int(self.geometry['direct_special_ratio']))
        pw, ph = (width + 31) // 32 * 32, (height + 31) // 32 * 32
        self.put(self.network, '6I', pw, ph, pw, ph, iw, ih)
        self.put(OBJ + 0x118, 'Q', self.backend + 0x400)
        self.put(self.backend + 0x420, 'I', selector)
        self.put(self.backend + 0x428, 'Q', 1000)
        self.phase = 'descriptor-init'
        invoke(self.uc, 0x63220, [(UC_X86_REG_RDI, OBJ), (UC_X86_REG_RSI, selector)])
        self.configs = []
        for stage, divisor in enumerate(DIVISORS):
            self.phase, self.stage = 'stage-allocation', stage
            base = self.network + 0x18 + stage * 0x5c8
            self.put(base, 'Q', stage + 1)
            self.put(base + 0x550, 'Q', self.backend)
            self.put(base + 0x5c0, 'B', 1)
            self.put(base + 0x5c4, 'I', selector)
            self.put(base + 0x5ac, 'I', self.get(base + 0x5b0, 'I'))
            rsp = STACK + 0xff00 - 8
            self.put(rsp, '4Q', STOP, OBJ + 0x140 + stage * 0x140, 10, 0x116560)
            self.uc.reg_write(UC_X86_REG_RSP, rsp)
            for reg, val in [(UC_X86_REG_RDI, base), (UC_X86_REG_RSI, self.state),
                             (UC_X86_REG_RDX, 0), (UC_X86_REG_RCX, pw // divisor),
                             (UC_X86_REG_R8, ph // divisor),
                             (UC_X86_REG_R9, self.state + 0xbe0 + stage * 0x98)]:
                self.uc.reg_write(reg, val)
            self.uc.emu_start(PACKERS[stage], STOP, count=20000000)
            assert self.uc.reg_read(UC_X86_REG_RIP) in range(0x71500, 0x71980, 0x80)
            weights = self.resource({'kind': 'weights', 'stage': stage})
            self.put(self.state + 0xbe0 + stage * 0x98 + 0x70, 'Q', weights)
            self.configs.append({'index': stage, 'shape': [pw // divisor, ph // divisor],
                                 'tiles': list(self.get(base + 0x594, 'II')),
                                 'window': list(self.get(base + 0x59c, 'II')),
                                 'block': [32, self.get(base + 0x594, 'I') * self.get(base + 0x598, 'I'),
                                           self.get(base + 0x5ac, 'I')]})
        # Persistent allocation descriptors are the literals in original 0x83100.
        for offset, name, w, h, fmt in [
            (0xa38, 'HiddenHistory', width, height, 26),
            (0xa40, 'InternalHistoryA', iw, ih, 2),
            (0xa48, 'InternalHistoryB', iw, ih, 2),
            (0xab8, 'PrevMvec1', width, height, 2),
            (0xac0, 'PrevMvec2', width, height, 2),
            (0xac8, 'PrevJitter1', width, height, 5),
            (0xad0, 'PrevJitter2', width, height, 5),
            (0xb90, 'Exposure0', 1, 1, 6), (0xb98, 'Exposure1', 1, 1, 6)]:
            handle = self.resource({'kind': 'texture', 'name': name, 'shape': [w, h], 'native_format': fmt})
            self.put(self.state + offset, 'Q', handle)
        if self.geometry['separate_downsample']:
            handle = self.resource({'kind': 'texture', 'name': 'OutputHistRes', 'shape': [iw, ih], 'native_format': 2})
            self.put(self.state + 0xba8, 'Q', handle)
        self.auxiliary = {
            1001: (12, 48, [8, 8, 1]), 1002: (8, 24, [256, 1, 1]),
            1003: (11, 24, [1, 1, 1]), 1004: (56, 88, [16, 16, 1]),
            1005: (9, 64, [8, 8, 1]), 1006: (10, 40, [1, 1, 1]),
        }
        for offset, handle in [(0x70, 1001), (0x78, 1002), (0x80, 1003),
                               (0x138, 1004), (0x20, 1005), (0x28, 1006)]:
            self.put(OBJ + offset, 'Q', handle)
        self.put(OBJ + 0x30, 'I', 1)
        exposure = self.state + 0x8a0
        # Original 0x2be70 clamps luma dimensions to at least 256 each.
        ew, eh = max(256, width // 2), max(256, height // 2)
        self.put(exposure + 0x11c, 'II', ew, eh)
        self.put(exposure + 0x104, 'f', 0.18)
        for offset, size in [(0x130, ew * eh * 4), (0x138, ew * eh * 4), (0x140, 128 * 4)]:
            handle = self.resource({'kind': 'tensor', 'name': f'exposure{offset:x}', 'bytes': size})
            self.put(exposure + offset, 'Q', handle)
        self.phase = 'ready'

    def put(self, address, fmt, *values):
        self.uc.mem_write(address, struct.pack('<' + fmt, *values))

    def get(self, address, fmt='Q'):
        values = struct.unpack('<' + fmt, self.uc.mem_read(address, struct.calcsize('<' + fmt)))
        return values[0] if len(values) == 1 else values

    def resource(self, descriptor):
        handle = len(self.resources) + 1
        self.resources[handle] = dict(descriptor, resource_id=handle)
        return handle

    def hook(self, machine, address, size, context):
        backend_call = self.backend + 0x2000 <= address < self.backend + 0x2300
        intercepted = (0x9c250, 0x9c770, 0x9d5a0, 0x10600, 0x10810, 0x60ed0,
                       0x17680, 0x71c80, 0x5e680, 0x5f090, 0x5d2e0, 0x5d1d0, 0x2e6e0)
        if not backend_call and address not in intercepted and address not in range(0x71500, 0x71980, 0x80):
            return
        a, b, c, d, e, f = [machine.reg_read(r) for r in
                            (UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_RDX,
                             UC_X86_REG_RCX, UC_X86_REG_R8, UC_X86_REG_R9)]
        stack = machine.reg_read(UC_X86_REG_RSP)
        target = self.get(stack)
        result = 0
        if address in range(0x71500, 0x71980, 0x80):
            machine.emu_stop()
            return
        if address == 0x9c250:
            result = self.cursor
            self.cursor = (self.cursor + a + 255) & ~255
            assert self.cursor < self.heap + 0x8000000
        elif address == 0x10810:
            result = bytes(machine.mem_read(a, 256)).index(0)
        elif address == 0x10600:
            machine.mem_write(a, bytes([b & 255]) * c)
            result = a
        elif address in (0x60ed0, 0x17680, 0x71c80):
            self.put(a, '4Q', a + 16, 0, 0, 0)
            result = a
        elif address == 0x5e680:
            handle = self.resource({'kind': 'tensor', 'bytes': d, 'stage': self.stage,
                                    'birth': e, 'death': f, 'origin': hex(target - 5)})
            self.put(a, 'QQ', handle, 0)
        elif address == 0x5d2e0:
            handle = self.get(b)
            self.put(c, 'Q', handle << 32)
            self.bindings.append({'resource': handle, 'handle': handle << 32, 'slot': 'arena',
                                  'parameter_address': c, 'callsite': hex(target - 5)})
        elif backend_call:
            slot = address - self.backend - 0x2000
            if slot == 0x80:
                desc = self.get(b, '8I')
                handle = self.resource({'kind': 'texture', 'stage': self.stage,
                                        'shape': list(desc[:2]), 'native_format': desc[3],
                                        'descriptor': list(desc), 'origin': hex(target - 5)})
                self.put(c, 'Q', handle)
            elif slot in (0xb0, 0xb8, 0xc0, 0xc8):
                mode = d if slot == 0xb0 else None
                key = (b, slot, mode)
                handle = (b << 32) if slot in (0xc0, 0xc8) else (0 if not b else self.views.setdefault(key, len(self.views) + 1))
                self.put(c, 'Q', handle)
                self.bindings.append({'resource': b, 'handle': handle, 'slot': hex(slot), 'mode': mode,
                                      'parameter_address': c, 'callsite': hex(target - 5)})
            elif slot == 0x198:
                resource = self.resources[b]
                self.put(c, '3I', *resource['shape'], 0x10 if resource['native_format'] in (2, 5, 6) else 0x20)
            elif slot == 0x30:
                self.put(c, '7I', {0x10: 2, 0x20: 1, 200: 26}[b], 0, 0, 0, 0, 0, 0)
            elif slot == 0xe0:
                self.bound_kernel = b
            elif slot == 0x148:
                index = self.bound_kernel - 1
                n = ((1032 if index == 0 else 152 if index == 10 else 144) if index < 11
                     else 480 if self.bound_kernel == 1000 else self.auxiliary[self.bound_kernel][1])
                params = bytes(machine.mem_read(b + 8, n))
                self.dispatches.append({'kernel': self.bound_kernel, 'parameter_address': b + 8,
                                        'params': params.hex(), 'grid': [d, e, f],
                                        'block': (self.configs[index]['block'] if index < 11 else [16, 16, 1]
                                                  if self.bound_kernel == 1000 else self.auxiliary[self.bound_kernel][2]),
                                        'bindings': [dict(x, parameter_offset=x['parameter_address'] - b - 8)
                                                     for x in self.bindings if b + 8 <= x['parameter_address'] < b + 8 + n]})
            else:
                assert slot in (0x48, 0xa8, 0x158, 0x168), (hex(slot), hex(target))
                result = 89 if slot == 0x48 else 0
        else:
            assert address in (0x9c770, 0x9d5a0, 0x5f090, 0x5d1d0, 0x2e6e0)
        machine.reg_write(UC_X86_REG_RAX, result)
        machine.reg_write(UC_X86_REG_RSP, stack + 8)
        machine.reg_write(UC_X86_REG_RIP, target)

    def frame(self, reset=True, parity=0, cycle=0, metadata=None):
        metadata = metadata or {}
        self.bindings.clear()
        self.dispatches.clear()
        self.put(self.state + 0x1658, 'I', parity)
        self.put(self.state + 0x1654, 'I', cycle)
        self.put(self.packet + 0x230, 'II', self.width, self.height)
        self.put(self.packet + 0x120, 'B', int(reset))
        self.put(self.packet + 0x128, 'ff', *metadata.get('motion_scale', (1.0, 1.0)))
        # Original public Evaluate builds this packet at local_3c8.
        self.put(self.packet + 0x130, 'ff', *metadata.get('motion_offset', (0.0, 0.0)))
        self.put(self.packet + 0x138, 'ff', *self.get(self.state + 0xb0, 'ff'))
        self.put(self.packet + 0x140, 'ff', *metadata.get('jitter', (0.0, 0.0)))
        self.put(self.packet + 0x148, 'ff', *metadata.get('jitter_slope', (0.0, 0.0)))
        self.put(self.packet + 0x150, 'f', metadata.get('sharpness', 0.5))
        self.put(self.packet + 0x238, 'ff', metadata.get('pre_exposure', 1.0),
                 metadata.get('exposure_scale', 1.0))
        identity = [float(i % 5 == 0) for i in range(16)]
        for current, previous, key in [(0x304, 0x2188, 'world_to_view'), (0x344, 0x21c8, 'view_to_clip')]:
            matrix = metadata.get(key, identity)
            self.put(self.packet + current, '16f', *matrix)
            if reset:
                self.put(self.state + previous, '16f', *matrix)
        for index in (0, 1, 2, 6, 9, 10, 11, 12):
            existing = self.get(self.packet + index * 8)
            if not existing:
                handle = self.resource({'kind': 'external', 'name': f'packet{index}',
                                        'shape': list(self.output_size) if index == 6 else [self.width, self.height],
                                        'native_format': 2})
                self.put(self.packet + index * 8, 'Q', handle)
        entries = [0x631d0, 0x614a0, 0x6d340, 0x86490]
        if self.geometry['separate_downsample']:
            entries.append(0x62f00)
        for entry in entries:
            self.phase = hex(entry)
            invoke(self.uc, entry, [(UC_X86_REG_RDI, OBJ), (UC_X86_REG_RSI, 0),
                                   (UC_X86_REG_RDX, self.state), (UC_X86_REG_RCX, self.packet)], limit=2000000)
            assert self.uc.reg_read(UC_X86_REG_RAX) == 1, (hex(entry), hex(self.uc.reg_read(UC_X86_REG_RAX)))
        assert [d['kernel'] for d in self.dispatches if d['kernel'] <= 1000] == list(range(1, 12)) + [1000]
        return {'selector': self.selector, 'width': self.width, 'height': self.height,
                'creation_flags': self.get(self.state + 0x90, 'I'),
                'exposure_policy': 'Original D/E uses internal auto exposure; packet ExposureTexture is not selected.',
                'reset': reset, 'parity': parity, 'cycle': cycle, 'configs': self.configs,
                'resources': self.resources, 'views': [{'resource': k[0], 'slot': hex(k[1]), 'mode': k[2], 'handle': v}
                                                       for k, v in self.views.items()],
                'dispatches': self.dispatches}


FORMATS = {1: ('f32', 4, 4), 2: ('f16', 4, 2), 5: ('f16', 2, 2),
           6: ('f16', 1, 2), 8: ('f32', 2, 4), 9: ('f32', 1, 4), 26: ('snorm8', 4, 1)}
INPUTS = {'Color': 0, 'MotionVectors': 1, 'Depth': 2, 'GBuffer.Normals': 9,
          'GBuffer.Roughness': 10, 'DLSS.Input.DiffuseAlbedo': 11, 'DLSS.Input.SpecularAlbedo': 12}
OPTIONAL_INPUTS = {'ExposureTexture': 5, 'DLSSD.Alpha': 7, 'DLSS.TransparencyLayer': 30,
                   'DLSS.TransparencyLayerOpacity': 31, 'DLSS.TransparencyLayerMvecs': 32}
INPUT_ORIGINS = {'Color': 0x168, 'Depth': 0x170, 'MotionVectors': 0x178, 'DLSSD.Alpha': 0x190,
                 'DLSS.Input.DiffuseAlbedo': 0x1a0, 'DLSS.Input.SpecularAlbedo': 0x1a8,
                 'GBuffer.Normals': 0x1b0, 'GBuffer.Roughness': 0x1b8, 'DLSS.TransparencyLayer': 0x1e0,
                 'DLSS.TransparencyLayerOpacity': 0x1e8, 'DLSS.TransparencyLayerMvecs': 0x1f0}
STAGES = ['enc0', 'enc1', 'enc2', 'enc3', 'enc4', 'enc5', 'dec4', 'dec3', 'dec2', 'dec1', 'dec0']
# Measured on the restored 32x32 workload: the middle stages amortize the
# checkpoint boundary, while the small edge stages do not. Keep this explicit
# until phase orchestration is fused; never hide a per-stage backend fallback.
TENSOR_STAGE_KERNELS = frozenset(range(3, 10))


class Model:
    """Standalone recovered D/E execution. Original ELF remains a host dependency.

    Host arithmetic executes original x86; every neural/pre/post instruction
    executes PTX-derived Metal via MLX. No NVIDIA image equivalence is implied.
    """
    def __init__(self, selector, inputs, packing=None, output_size=None, multiscale=False,
                 input_size=None, input_origins=None, alpha_upscaling=False, backend='oracle'):
        if backend not in ('oracle', 'optimized'):
            raise ValueError('backend must be oracle or optimized')
        self.backend = backend
        if selector not in (4, 5) or not set(INPUTS) <= set(inputs) or set(inputs) - (INPUTS.keys() | OPTIONAL_INPUTS.keys()):
            raise ValueError('Select D=4/E=5 and provide all seven required guide arrays')
        if not isinstance(inputs['Color'], np.ndarray) or inputs['Color'].ndim != 3:
            raise ValueError('Color: expected an HWC NumPy array')
        height, width = inputs['Color'].shape[:2]
        if input_size is not None:
            if len(input_size) != 2 or any(type(v) is not int for v in input_size):
                raise ValueError('input_size must contain integer width and height')
            width, height = input_size
        if min(width, height) <= 0:
            raise ValueError('Input dimensions must be positive')
        origins = input_origins or {}
        if set(origins) - (inputs.keys() & INPUT_ORIGINS.keys()):
            raise ValueError('Subrect origin requires a matching input guide')
        self.input_contract, self.input_crops = {}, {}
        for name, array in inputs.items():
            channels = (2 if name in ('MotionVectors', 'DLSS.TransparencyLayerMvecs') else
                        1 if name in ('Depth', 'GBuffer.Roughness', 'ExposureTexture', 'DLSSD.Alpha',
                                      'DLSS.TransparencyLayerOpacity') else 4)
            shape = (1, 1, channels) if name == 'ExposureTexture' else (height, width, channels)
            origin = origins.get(name, (0, 0))
            if len(origin) != 2 or any(type(v) is not int or v < 0 for v in origin):
                raise ValueError(f'{name}: subrect origin must be two nonnegative integers')
            if (not isinstance(array, np.ndarray) or array.ndim != 3 or array.shape[2] != channels
                    or (input_size is None and array.shape != shape)
                    or (name == 'ExposureTexture' and array.shape != shape)
                    or origin[0] + shape[1] > array.shape[1] or origin[1] + shape[0] > array.shape[0]):
                raise ValueError(f'{name}: expected HWC shape {shape}')
            if array.dtype not in (np.float16, np.float32) or not np.isfinite(array).all():
                raise ValueError(f'{name}: finite float16/float32 input required')
            self.input_contract[name] = (array.shape, array.dtype)
            # HKPN samples these two inputs over the whole normalized texture
            # (module27:3304-3312), without their public subrect origins. Bind
            # an explicit MLX ROI copy so its original sampling remains intact.
            if (name in ('DLSS.TransparencyLayer', 'DLSS.TransparencyLayerOpacity')
                    and (array.shape[:2] != (height, width) or origin != (0, 0))):
                self.input_crops[name] = origin
        self.host = Host(selector, width, height, output_size, multiscale)
        self.host.put(self.host.state + 0x90, 'I', 3 | (0x80 if alpha_upscaling else 0))
        # Both accepted color formats (RGBA16F/RGBA32F) pass original 0x187053.
        self.host.put(self.host.state + 0xa05, 'BB', int(alpha_upscaling), int(alpha_upscaling))
        self.host.frame()
        for name in inputs.keys() & OPTIONAL_INPUTS.keys():
            index = OPTIONAL_INPUTS[name]
            h, w = inputs[name].shape[:2]
            handle = self.host.resource({'kind': 'external', 'name': name, 'shape': [w, h], 'native_format': 2})
            self.host.put(self.host.packet + index * 8, 'Q', handle)
        if 'DLSSD.Alpha' in inputs:
            handle = self.host.resource({'kind': 'texture', 'name': 'OutputAlpha',
                                         'shape': list(self.host.output_size), 'native_format': 6})
            self.host.put(self.host.packet + 64, 'Q', handle)
        for key, offsets in [('DLSS.TransparencyLayer', (0xa60, 0xa68)),
                              ('DLSS.TransparencyLayerOpacity', (0xa70, 0xa78))]:
            if key in inputs:
                for offset in offsets:
                    handle = self.host.resource({'kind': 'texture', 'name': f'History{offset:x}',
                                                 'shape': self.host.geometry['internal_output'], 'native_format': 2})
                    self.host.put(self.host.state + offset, 'Q', handle)
        self.selector, self.width, self.height = selector, width, height
        self.parity = self.cycle = 0
        self.input_handles = {name: self.host.get(self.host.packet + (INPUTS | OPTIONAL_INPUTS)[name] * 8) for name in inputs}
        self.output_handle = self.host.get(self.host.packet + 48)
        for name, array in inputs.items():
            fmt = ({1: 6, 2: 5, 4: 2} if array.dtype == np.float16 else {1: 9, 2: 8, 4: 1})[array.shape[2]]
            shape = [width, height] if name in self.input_crops else list(array.shape[1::-1])
            self.host.resources[self.input_handles[name]].update(native_format=fmt, shape=shape)
            if name in INPUT_ORIGINS:
                origin = (0, 0) if name in self.input_crops else origins.get(name, (0, 0))
                self.host.put(self.host.packet + INPUT_ORIGINS[name], 'II', *origin)
        # The CUDA resource owns these views already. Reserve their tokens once
        # so changing history parity does not change the generated kernel ABI.
        for handle, resource in self.host.resources.items():
            if 'native_format' in resource:
                for slot, mode in ((0xb0, 0), (0xb0, 2), (0xb8, None)):
                    self.host.views.setdefault((handle, slot, mode), len(self.host.views) + 1)
        packing = Path(packing or ROOT.parent / 'dlss-rr-mlx-20261011/packing')
        manifest = json.loads((packing / 'manifest.json').read_text())
        assert manifest['pass'] and manifest['source']['sha256'] == SOURCE_SHA
        model = next(m for m in manifest['models'] if m['selector'] == selector)
        self.spans, self.guards = {}, []
        arena, weights = bytearray(), bytearray()
        self.weight_hashes = []
        for handle, resource in self.host.resources.items():
            if resource['kind'] == 'weights':
                stage = model['stages'][resource['stage']]
                data = (packing / stage['file']).read_bytes()
                assert hashlib.sha256(data).hexdigest() == stage['sha256']
                self.spans[handle] = dict(resource, buffer='weights', arena_offset=len(weights),
                                          bytes=len(data), virtual_base=handle << 32)
                weights.extend(data)
                self.weight_hashes.append(stage['sha256'])
                continue
            descriptor = dict(resource, buffer='arena', virtual_base=handle << 32)
            if 'native_format' in resource:
                dtype, channels, channel_bytes = FORMATS[resource['native_format']]
                w, h = resource['shape']
                descriptor.update(shape=[w, h, 1, channels],
                    storage={'dtype': dtype, 'channel_bytes': channel_bytes,
                             'row_pitch': w * channels * channel_bytes, 'pixel_stride': channels * channel_bytes},
                    bytes=w * h * channels * channel_bytes)
            padding = (-len(arena)) % 16
            arena.extend(bytes(padding))
            self.guards.append((len(arena), 64))
            arena.extend(b'\xa5' * 64)
            descriptor['arena_offset'] = len(arena)
            arena.extend(bytes(descriptor['bytes']))
            self.guards.append((len(arena), 64))
            arena.extend(b'\xa5' * 64)
            self.spans[handle] = descriptor
        self.global_symbols = {'Jitter': {'virtual_base': 0xffff00000000, 'arena_offset': len(arena),
                                           'bytes': 8, 'buffer': 'arena'}}
        arena.extend(bytes(8))
        self.arena = mx.array(np.frombuffer(arena, dtype=np.uint8))
        self.weights = mx.array(np.frombuffer(weights, dtype=np.uint8))
        self.constants = mx.array([], dtype=mx.uint8)
        self.modules = json.loads((ROOT / 'reconstruction/cuda/modules.json').read_text())['modules']
        self.compiled = {}

    def dispatch_spec(self, dispatch, capture):
        kernel = dispatch['kernel']
        if kernel <= 11:
            index = kernel - 1
            module = ([25, 14, 15, 16, 17, 18, 19, 20, 21, 22, 26] if self.selector == 4 else list(range(13, 24)))[index]
            suffix = '_diamond_wallaby' if self.selector == 4 and index in (0, 10) else ''
            entry = f'cuda_dldn_engine_swin_{STAGES[index]}{suffix}_kernel'
        elif kernel == 1000:
            module = 27 if self.selector == 4 else 24
            entry = 'cuda_dldn_engine_hkpn_output_kernel_transformer' + ('_diamond_wallaby' if self.selector == 4 else '')
        else:
            module = self.host.auxiliary[kernel][0]
            entry = self.modules[module]['entries'][0]
        resources = []
        for handle, span in self.spans.items():
            resources.append(dict(span, handle=handle << 32, view={'kind': 'global'},
                                  access='read' if span['buffer'] == 'weights' else 'readwrite'))
        for view in capture['views']:
            span = self.spans[view['resource']]
            resources.append(dict(span, handle=view['handle'], access='readwrite',
                                  view={'kind': 'texture' if view['slot'] == '0xb0' else 'surface',
                                        'filter': 'linear' if view['mode'] == 0 else 'point',
                                        'normalized_coordinates': True, 'address_mode': 'clamp',
                                        'channel_read_mode': 'normalized' if span['storage']['dtype'] == 'snorm8' else 'element'}))
        return {'schema': 'rr.mlx.dispatch.v1', 'model_selector': self.selector,
                'module': f'module-{module:02d}.ptx', 'entry': entry,
                'params': bytes.fromhex(dispatch['params']), 'cuda_grid': dispatch['grid'],
                'cuda_block': dispatch['block'], 'resources': resources, 'global_symbols': self.global_symbols,
                'state_before': {key: capture[key] for key in ('parity', 'cycle', 'reset')},
                'source_locations': dispatch['bindings']}

    def evaluate(self, inputs, reset=False, metadata=None, capture_directory=None):
        from . import ptx
        from . import lifted

        if set(inputs) != set(self.input_contract):
            raise ValueError('All required guides must be supplied each frame')
        for name, array in inputs.items():
            if (not isinstance(array, np.ndarray) or (array.shape, array.dtype) != self.input_contract[name]
                    or not np.isfinite(array).all()):
                raise ValueError(f'{name}: input shape/type changed or is nonfinite; recreate model on resize')
        arena = self.arena
        for name, array in inputs.items():
            span = self.spans[self.input_handles[name]]
            start, size = span['arena_offset'], span['bytes']
            data = mx.array(np.ascontiguousarray(array).view(np.uint8))
            if name in self.input_crops:
                x, y = self.input_crops[name]
                data = data[y:y + self.height, x:x + self.width, :]
            data = data.reshape(-1)
            arena = mx.concatenate([arena[:start], data, arena[start + size:]])
        capture = self.host.frame(reset, self.parity, self.cycle, metadata)
        directory = Path(capture_directory) if capture_directory is not None else None
        if directory is not None:
            directory.mkdir(parents=True, exist_ok=True)
            (directory / 'host-capture.json').write_text(json.dumps(
                {'capture': capture, 'spans': self.spans, 'global_symbols': self.global_symbols,
                 'input_roi_copies': self.input_crops}, indent=2) + '\n')
        receipts = []
        for number, dispatch in enumerate(capture['dispatches']):
            spec = self.dispatch_spec(dispatch, capture)
            source = (ROOT / 'reconstruction/cuda' / spec['module']).read_text()
            module_index = int(spec['module'][7:9])
            assert hashlib.sha256(source.encode()).hexdigest() == self.modules[module_index]['decoded_sha256']
            original_sha = hashlib.sha256(source.encode()).hexdigest()
            repair = None
            if dispatch['kernel'] == 1002:
                # The shipped reduction reads outside its 8-lane membermask.
                # PTX specifies undefined results there. Repair that auxiliary
                # reduction's range, never the generic shuffle implementation.
                before = '@%p3 bra $L__BB0_13;\n\n$L__BB0_12:'
                assert source.count(before) == 1 and source.count('mov.u32 %r38, 31;') == 1
                source = source.replace(before, '@%p3 bra $L__BB0_13;\nmov.u32 %r43, %r42;\n\n$L__BB0_12:')
                source = source.replace('mov.u32 %r38, 31;', 'add.s32 %r38, %r42, -1;')
                repair = 'Original undefined partial-warp auto-exposure sum: start at participating width and clamp to its last lane.'
            elif dispatch['kernel'] <= 5:
                # Original enc0..enc4 crop live lanes before full-mask reductions.
                # Its even padded extents/offsets make the crop a union of 2x2
                # pairs. Preserve every published half-arithmetic dependency,
                # but define participation and unused odd-lane shuffle results.
                dimensions = struct.unpack_from('<II', spec['params'])
                offsets = struct.unpack_from('<HH', spec['params'], 80)
                assert all(v % 2 == 0 for v in (*dimensions, *offsets))
                sites = {
                    13: [(44699, 466, 307, [45076, 45091, 45110, 45134]),
                         (45191, 474, 310, [45570, 45585, 45604, 45628])],
                    25: [(43986, 428, 278, [44364, 44379, 44398, 44422]),
                         (44479, 436, 281, [44859, 44874, 44893, 44917])],
                    14: [(67117, 519, 438, [67548, 67563, 67582, 67606]),
                         (67662, 539, 441, [68095, 68110, 68129, 68153])],
                    15: [(34020, 265, 260, [34463, 34478, 34497, 34521])],
                    16: [(42054, 333, 340, [42497, 42512, 42531, 42555])],
                    17: [(52741, 403, 422, [53185, 53200, 53219, 53243])],
                }[module_index]
                lines = source.splitlines(keepends=True)
                for group, (branch, predicate, label, shuffles) in enumerate(sites):
                    mask = f'%rr_crop_mask{group}'
                    assert lines[branch - 1].strip() == f'@%p{predicate} bra $L__BB1_{label};'
                    lines[branch - 1] = (f'.reg .b32 {mask};\nvote.sync.ballot.b32 {mask}, !%p{predicate}, -1;\n'
                                          + lines[branch - 1])
                    for line, control in zip(shuffles, (0x1e1f, 0x171f, 0x1e1f, 0x171f)):
                        assert lines[line - 1].startswith('{shfl.sync.down.b32 ')
                        prefix, _, _ = lines[line - 1].rsplit(',', 2)
                        lines[line - 1] = f'{prefix},{control},{mask};\n'
                source = ''.join(lines)
                repair = 'Original undefined cropped full-warp pooling shuffles: ballot participants and 2x2 pair clamps; published half arithmetic unchanged. NVIDIA undefined behavior not claimed.'
            tensor = self.backend == 'optimized' and dispatch['kernel'] in TENSOR_STAGE_KERNELS
            key = (self.backend, spec['entry'], tuple(spec['cuda_grid']),
                   tuple(spec['cuda_block']), tuple(v['handle'] for v in spec['resources']))
            if key not in self.compiled:
                self.compiled[key] = (lifted.compile_lifted(source, spec['entry'], spec) if tensor else
                                      ptx.compile_kernel(source, spec['entry'], spec))
            compiled = self.compiled[key]
            emitted = compiled['resume'] if tensor else compiled
            if directory is not None:
                if repair is not None:
                    (directory / f'{number:02d}-executed.ptx').write_text(source)
                (directory / f'{number:02d}-params.bin').write_bytes(spec['params'])
                (directory / f'{number:02d}-source.metal').write_text(emitted['header'] + '\n' + emitted['source'])
                (directory / f'{number:02d}-program.bin').write_bytes(emitted['program'])
                (directory / f'{number:02d}-source-map.json').write_text(json.dumps(emitted['source_map'], indent=2) + '\n')
                if tensor:
                    (directory / f'{number:02d}-tensor-coverage.json').write_text(json.dumps(compiled['coverage'], indent=2) + '\n')
            print(f"MLX {number + 1}/{len(capture['dispatches'])} {spec['entry']}", flush=True)
            before = np.asarray(arena)
            started = perf_counter()
            arena, receipt = (lifted.execute_lifted(spec, compiled, arena, self.weights, self.constants) if tensor else
                              runtime.execute(spec, compiled, arena, self.weights, self.constants))
            receipt['gpu_execution_seconds'] = perf_counter() - started
            receipt.setdefault('backend', 'ptx_oracle')
            receipt['optimization_selection'] = ('lifted_mma' if tensor else
                                                 'oracle_measured_boundary_overhead'
                                                 if self.backend == 'optimized' and dispatch['kernel'] <= 11
                                                 else 'oracle')
            raw = np.asarray(arena)
            for start, size in self.guards:
                assert np.all(raw[start:start + size] == 0xa5), ('arena canary', spec['entry'], start)
            receipt['arena_sha256'] = hashlib.sha256(raw.tobytes()).hexdigest()
            receipt['parameter_sha256'] = hashlib.sha256(spec['params']).hexdigest()
            receipt['changed_resources'] = []
            for handle, span in self.spans.items():
                if span['buffer'] != 'arena':
                    continue
                start, size = span['arena_offset'], span['bytes']
                data = raw[start:start + size]
                changed = int(np.count_nonzero(data != before[start:start + size]))
                if changed:
                    item = {'resource_id': handle, 'shape': span.get('shape'), 'bytes': size,
                            'changed_bytes': changed, 'sha256': hashlib.sha256(data.tobytes()).hexdigest()}
                    dtype = span.get('storage', {}).get('dtype')
                    if dtype in ('f16', 'f32'):
                        values = data.view(np.float16 if dtype == 'f16' else np.float32)
                        item['storage_finite_values'] = int(np.isfinite(values).sum())
                        item['storage_values'] = values.size
                    receipt['changed_resources'].append(item)
            receipt['ptx_sha256'] = original_sha
            receipt['executed_ptx_sha256'] = hashlib.sha256(source.encode()).hexdigest()
            receipt['semantic_repair'] = repair
            receipt['opcode_coverage'] = (compiled['oracle'] if tensor else compiled)['opcode_coverage']
            receipts.append(receipt)
            if directory is not None:
                np.save(directory / f'{number:02d}-{spec["entry"]}.npy', raw)
                (directory / f'{number:02d}-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        output = self.spans[self.output_handle]
        start, size = output['arena_offset'], output['bytes']
        image = np.asarray(arena)[start:start + size].copy().view(np.float16).reshape(output['shape'][1], output['shape'][0], 4)
        assert np.isfinite(image).all(), 'Nonfinite reconstructed output'
        self.arena = arena
        invoke(self.host.uc, 0x63180, [(UC_X86_REG_RDI, OBJ), (UC_X86_REG_RSI, self.host.state)])
        self.parity, self.cycle = 1 - self.parity, (self.cycle + 1) % 8
        self.host.put(self.host.state + 0xb0, 'ff', *self.host.get(self.host.packet + 0x140, 'ff'))
        for current, previous in [(0x304, 0x2188), (0x344, 0x21c8)]:
            self.host.uc.mem_write(self.host.state + previous, bytes(self.host.uc.mem_read(self.host.packet + current, 64)))
        return image, receipts
