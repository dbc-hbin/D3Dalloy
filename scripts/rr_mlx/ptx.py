# SPDX-License-Identifier: Apache-2.0
# Derived from iamwavecut/MLX-DLSS ptx_metal.py at
# 0ca2deab092fe6f3e331bf4f616271dbc64521d0. Original upstream is Apache-2.0.
"""Source-derived RR PTX compiler. Unsupported syntax fails before dispatch.

This emitter keeps PTX registers as bits and executes immutable instruction
records in a uniform CTA scheduler. No PTX instruction runs in Python during inference.
"""

from collections import Counter
from dataclasses import dataclass
import hashlib
import re
import struct
from bisect import bisect_right
from itertools import groupby
from types import SimpleNamespace


class CompileError(ValueError):
    """An original source location cannot be translated faithfully."""


_SCALAR_OPCODES = frozenset("""
abs.f16x2 abs.ftz.f32 abs.s32
add.f16 add.f16x2 add.ftz.f32 add.rz.ftz.f32 add.s32 add.s64
and.b16 and.b32 and.b64 and.pred bfi.b32
cvt.f32.f16 cvt.rmi.ftz.f32.f32 cvt.rn.f16.f32 cvt.rn.f16.f64
cvt.rn.f16.s32 cvt.rn.f16x2.e4m3x2 cvt.rn.f32.s32 cvt.rn.f32.u32
cvt.rn.f32.u64 cvt.rn.satfinite.e4m3x2.f16x2
cvt.rn.satfinite.relu.e4m3x2.f16x2 cvt.rni.ftz.u64.f32
cvt.rpi.ftz.f32.f32 cvt.rz.f16.f32 cvt.rzi.f32.f32
cvt.rzi.ftz.s32.f32 cvt.rzi.ftz.u32.f32 cvt.s64.s32 cvt.u16.u32
cvt.u32.u16 cvt.u32.u64 cvt.u64.u32 cvta.to.global.u64
div.approx.ftz.f32 div.s32 div.u16 div.u32 ex2.approx.ftz.f32
cvt.ftz.f64.f32 cvt.rn.ftz.f32.f64 cvt.ftz.sat.f32.f32 max.f64 div.rn.f64
vote.sync.ballot.b32 atom.global.add.f32 atom.shared.add.f32
fma.rn.f16 fma.rn.f16x2 fma.rn.f32 fma.rn.ftz.f32
lg2.approx.ftz.f32 mad.lo.s32
max.f16 max.f16x2 max.ftz.f32 max.s32
min.f16x2 min.ftz.f32 min.s32 min.u32
mov.b16 mov.b32 mov.b64 mov.f32 mov.f64 mov.pred mov.u16 mov.u32 mov.u64
mul.f16 mul.f16x2 mul.ftz.f32 mul.hi.s32 mul.lo.s16 mul.lo.s32
mul.wide.s32 mul.wide.u16 mul.wide.u32 neg.f16 neg.f16x2 neg.ftz.f32
not.b32 not.pred or.b16 or.b32 or.pred rcp.approx.ftz.f32 rem.u32
rsqrt.approx.ftz.f32 selp.b32 selp.f32 selp.u16 selp.u32
set.eq.f16.f16 set.nan.f16.f16 shl.b16 shl.b32 shl.b64
shr.s32 shr.u16 shr.u32 shr.u64 sqrt.approx.ftz.f32 sub.f16
sub.f16x2 sub.ftz.f32 sub.s16 sub.s32 trap brkpt xor.pred
""".split())


def _supported(op):
    if op in _SCALAR_OPCODES or op in ("bra", "bra.uni", "ret"):
        return True
    # These modifiers are semantic, not an opcode-prefix acceptance shortcut.
    if re.fullmatch(r"(?:ld\.(?:(?:weak\.)?global\.(?:ca\.)?|param\.|shared\.|const\.)|st\.(?:global\.|shared\.))(?:v[24]\.)?[busf](?:8|16|32|64)",op):
        return True
    if re.fullmatch(r"setp\.(?:eq|ne|lt|le|gt|ge|equ|neu|ltu|leu|gtu|geu|lo|ls|hi|hs|num|nan)\.(?:(?:and|or|xor)\.)?(?:ftz\.)?(?:[bsu](?:16|32|64)|f(?:16|32))",op):
        return True
    if re.fullmatch(r"(?:tex\.(?:base\.|level\.)?|tld4\.[rgba]\.)2d\.v4\.f(?:16|32)\.(?:f32|s32|u32)",op):
        return True
    if re.fullmatch(r"(?:su(?:ld|st)\.b\.2d\.(?:v[24]\.)?b(?:8|16|32|64)|sust\.p\.2d\.(?:v[24]\.)?b32)\.(?:zero|clamp|trap)",op):
        return True
    return op in ("bar.sync","mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16","movmatrix.sync.trans.aligned.m8n8.b16","shfl.sync.up.b32","shfl.sync.down.b32","shfl.sync.bfly.b32","shfl.sync.idx.b32","atom.global.add.u32","atom.global.add.u64","atom.shared.add.u32","membar.gl","membar.cta","membar.sys")


@dataclass(frozen=True)
class Instruction:
    opcode: str
    args: tuple
    predicate: str | None
    line: int
    end_line: int
    offset: int
    end_offset: int
    text: str


def _split(text):
    result, start, depth = [], 0, 0
    for i, char in enumerate(text):
        if char in "{[(":
            depth += 1
        elif char in "}])":
            depth -= 1
        elif char == "," and depth == 0:
            result.append(text[start:i].strip())
            start = i + 1
    result.append(text[start:].strip())
    return result


def _clean(source):
    # Preserve offsets and lines, including block comments and trailing NUL.
    return re.sub(r"/\*.*?\*/|//[^\n]*|\x00", lambda m: re.sub(r"[^\n]", " ", m[0]), source, flags=re.S)


class Parser:
    def __init__(self, source, entry):
        self.source = source
        text = _clean(source)
        newlines = [i for i, c in enumerate(source) if c == "\n"]
        entries = list(re.finditer(r"(?:\.visible\s+)?\.entry\s+([\w$]+)\s*\(", text))
        chosen = [m for m in entries if m[1] == entry]
        if len(chosen) != 1:
            raise CompileError(f"Expected exactly one entry {entry!r}; available: {[m[1] for m in entries]}")
        match = chosen[0]
        start = text.find("{", match.end())
        depth, end = 1, start + 1
        while end < len(text) and depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        if depth:
            raise CompileError(f"Unclosed entry {entry}")
        signature = text[match.end():start]
        params = re.findall(r"\.param\s+(?:\.align\s+(\d+)\s+)?\.(\w+)\s+([\w$]+)(?:\[(\d+)\])?", signature)
        self.symbols, self.param_bytes = {}, 0
        for align, dtype, name, size in params:
            alignment = int(align or re.search(r"\d+", dtype)[0]) if align else int(re.search(r"\d+", dtype)[0]) // 8
            self.param_bytes = (self.param_bytes + alignment - 1) // alignment * alignment
            self.symbols[name] = 0x1000000000000000 + self.param_bytes
            self.param_bytes += int(size or 1) * int(re.search(r"\d+", dtype)[0]) // 8
        if not params:
            raise CompileError(f"Entry {entry} has no parameter declarations")
        self.registers, self.shared, self.labels, self.instructions = {}, [], {}, []
        self.declarations = []
        self.globals = []
        for m in re.finditer(r"\.(global|const)\s+(?:\.align\s+(\d+)\s+)?\.(\w+)\s+([\w$]+)(?:\[(\d+)\])?(?:\s*=\s*([^;]+))?\s*;", text[:match.start()]):
            self.globals.append((m[4], int(m[5] or 1) * int(re.search(r"\d+", m[3])[0]) // 8, m[6]))
        scopes, scope_id = [{}], 0
        i = start + 1
        while i < end - 1:
            if text[i].isspace():
                i += 1
                continue
            if text[i] in "{}":
                if text[i] == "{":
                    scope_id += 1
                    scopes.append({})
                else:
                    scopes.pop()
                i += 1
                continue
            label = re.compile(r"([\w$]+):").match(text, i)
            if label:
                if label[1] in self.labels:
                    raise CompileError(f"Duplicate label {label[1]}")
                self.labels[label[1]] = len(self.instructions)
                i = label.end()
                continue
            j, nested = i, 0
            while j < end - 1:
                if text[j] == ";" and nested == 0:
                    break
                if text[j] in "{[":
                    nested += 1
                elif text[j] in "}]":
                    nested -= 1
                j += 1
            if j == end - 1:
                raise CompileError(f"Unterminated PTX statement at line {source.count(chr(10), 0, i) + 1}: {text[i:j][:100]}")
            statement = text[i:j].strip()
            line = bisect_right(newlines, i) + 1
            if statement.startswith("."):
                reg = re.fullmatch(r"\.reg\s*\.(\w+)\s+(.+)", statement, flags=re.S)
                shared = re.fullmatch(r"\.shared\s+(?:\.align\s+(\d+)\s+)?\.(\w+)\s+([\w$]+)(?:\[(\d+)\])?", statement)
                if reg:
                    for declaration in _split(reg[2]):
                        array = re.fullmatch(r"([%\w$]+)<(\d+)>", declaration)
                        names = [f"{array[1]}{n}" for n in range(int(array[2]))] if array else [declaration]
                        for name in names:
                            renamed = f"s{scope_id}_" + re.sub(r"\W", "_", name)
                            scopes[-1][name] = renamed
                            self.registers[renamed] = reg[1]
                elif shared:
                    alignment = int(shared[1] or 1)
                    offset = sum(s[2] for s in self.shared)
                    offset = (offset + alignment - 1) // alignment * alignment
                    size = int(shared[4] or 1) * int(re.search(r"\d+", shared[2])[0]) // 8
                    self.symbols[shared[3]] = 0x20000000 + offset
                    # Store allocation extent including alignment padding.
                    previous = sum(s[2] for s in self.shared)
                    self.shared.append((shared[3], offset, size + offset - previous))
                elif re.fullmatch(r'.pragma "used_bytes_mask \d+"', statement):
                    pass  # Compiler metadata, not an executable operation.
                else:
                    raise CompileError(f"Unknown entry declaration line {line}: {statement}")
                self.declarations.append({"line": line, "offset": i, "end_offset": j + 1, "text": statement})
            else:
                pred = None
                if statement.startswith("@"):
                    pred, statement = statement[1:].split(None, 1)
                op, *tail = statement.split(None, 1)
                args = _split(tail[0]) if tail else []
                def resolve(token):
                    def replace(m):
                        for scope in reversed(scopes):
                            if m[0] in scope:
                                return scope[m[0]]
                        if m[0].startswith("%") and not re.fullmatch(r"%(?:tid|ntid|ctaid|nctaid)\.[xyz]|%laneid|%warpid|%nwarpid", m[0]):
                            raise CompileError(f"Undeclared register {m[0]} at line {line}")
                        return m[0]
                    return re.sub(r"%[\w$]+(?:\.[xyz])?|(?<![\w$])[\w$]+", replace, token)
                args = tuple(resolve(a) for a in args)
                pred = resolve(pred) if pred else None
                self.instructions.append(Instruction(op, args, pred, line, bisect_right(newlines, j) + 1, i, j + 1, source[i:j + 1]))
            i = j + 1
        self.shared_bytes = sum(s[2] for s in self.shared)
        if not self.instructions:
            raise CompileError(f"Empty entry {entry}")


_CONTEXT = r"""
struct rr_context {
  device uchar* arena; const device uchar* weights; const device uchar* constants;
  const device uchar* params; const device ulong* resources; const device uint* launch;
  device uint* diagnostics; threadgroup uchar* shared;
  threadgroup atomic_uint* defined; uint instruction; uint cta;
};
inline void rr_fault(thread rr_context& c, uint bits) {
  uint old=atomic_fetch_or_explicit((device atomic_uint*)c.diagnostics,bits,memory_order_relaxed);
  if(old==0) {c.diagnostics[1]=c.instruction; c.diagnostics[2]=c.cta;}
}
inline ulong rr_bytes(const device uchar* p, ulong offset, uint n) {
  ulong result=0; for(uint i=0;i<n;++i) result|=ulong(p[offset+i])<<(8*i); return result;
}
inline void rr_put(device uchar* p, ulong offset, ulong value, uint n) {
  for(uint i=0;i<n;++i) p[offset+i]=uchar(value>>(8*i));
}
inline ulong rr_buffer_load(thread rr_context& c,uint kind,ulong offset,uint n) {
  ulong size=c.launch[kind==0?6:kind==1?7:8];
  if(offset>size || n>size-offset){rr_fault(c,1);return ~0ul;}
  if(kind==0)return rr_bytes(c.arena,offset,n);
  if(kind==1)return rr_bytes(c.weights,offset,n);
  return rr_bytes(c.constants,offset,n);
}
inline void rr_buffer_store(thread rr_context& c,uint kind,ulong offset,ulong value,uint n) {
  if(kind!=0 || offset>c.launch[6] || n>ulong(c.launch[6])-offset){rr_fault(c,1);return;}
  rr_put(c.arena,offset,value,n);
}
inline ulong rr_load(thread rr_context& c,ulong address,uint n,uint space) {
  if(space==1) {
    ulong a=address-0x1000000000000000ul;
    if(a>c.launch[9] || n>ulong(c.launch[9])-a){rr_fault(c,1);return ~0ul;}
    return rr_bytes(c.params,a,n);
  }
  if(space==2) {
    ulong a=address-0x20000000ul;
    if(a>RR_SHARED_BYTES || n>RR_SHARED_BYTES-a){rr_fault(c,2);return ~0ul;}
    ulong result=0;
    for(uint i=0;i<n;++i) {
      if(!(atomic_load_explicit(c.defined+(a+i)/32,memory_order_relaxed)&(1u<<((a+i)%32)))) rr_fault(c,128);
      result|=ulong(c.shared[a+i])<<(8*i);
    }
    return result;
  }
  for(uint i=0;i<c.launch[10];++i) {
    const device ulong* r=c.resources+5*i;
    if(address>=r[1] && address-r[1]<=r[3] && n<=r[3]-(address-r[1]))
      return rr_buffer_load(c,uint(r[4]),r[2]+address-r[1],n);
  }
  RR_GLOBAL_LOAD
  rr_fault(c,1);return ~0ul;
}
inline void rr_store(thread rr_context& c,ulong address,ulong value,uint n,uint space) {
  if(space==2) {
    ulong a=address-0x20000000ul;
    if(a>RR_SHARED_BYTES || n>RR_SHARED_BYTES-a){rr_fault(c,2);return;}
    for(uint i=0;i<n;++i) {
      c.shared[a+i]=uchar(value>>(8*i));
      atomic_fetch_or_explicit(c.defined+(a+i)/32,1u<<((a+i)%32),memory_order_relaxed);
    }
    return;
  }
  for(uint i=0;i<c.launch[10];++i) {
    const device ulong* r=c.resources+5*i;
    if(address>=r[1] && address-r[1]<=r[3] && n<=r[3]-(address-r[1])) {
      rr_buffer_store(c,uint(r[4]),r[2]+address-r[1],value,n);return;
    }
  }
  RR_GLOBAL_STORE
  rr_fault(c,1);
}
inline float2 rr_ftz(float2 x) {return float2(rr_ftz(x.x),rr_ftz(x.y));}
inline ulong rr_float_integer(float x,uint bits,bool is_signed) {
  if(isnan(x))return 0ul;
  float bound=ldexp(1.0f,is_signed?int(bits)-1:int(bits));
  ulong upper=is_signed?((1ul<<(bits-1))-1ul):(bits==64?~0ul:(1ul<<bits)-1ul);
  if(x>=bound)return upper;
  if(is_signed){
    if(x<=-bound)return 1ul<<(bits-1);
    return ulong(long(x));
  }
  if(x<=0.0f)return 0ul;return ulong(x);
}
inline uint rr_bfi(uint a,uint b,uint position,uint count) {
  position&=255; count=min(count&255u,32u);
  if(position>=32 || count==0)return b;
  uint mask=(count==32?~0u:((1u<<count)-1u))<<position;
  return (b&~mask)|((a<<position)&mask);
}
"""


def _half_literal(literal):
    number = struct.unpack(">d", bytes.fromhex(literal[2:]))[0]
    try:
        return int.from_bytes(struct.pack("<e", number), "little")
    except OverflowError:
        return 0xfc00 if number < 0 else 0x7c00


class Emitter:
    def __init__(self, parser, spec):
        self.p, self.spec = parser, spec
        self.register_slots = {name: index for index, name in enumerate(parser.registers)}
        self.reads, self.writes = {}, {}
        self.folded = {}
        for inst in parser.instructions:
            if inst.opcode == "mov.f64":
                if inst.predicate or not re.fullmatch(r"0d[0-9A-Fa-f]{16}", inst.args[1]):
                    raise CompileError(f"Nonconstant f64 at line {inst.line}")
                self.folded.setdefault(inst.args[0], set()).add(inst.args[1])
        self.folded = {k: next(iter(v)) for k, v in self.folded.items() if len(v) == 1}
        for inst in parser.instructions:
            if inst.opcode != "mov.f64" and inst.args and inst.args[0] in self.folded:
                self.folded.pop(inst.args[0])
        self.global_load, self.global_store = [], []
        for name, size, initializer in parser.globals:
            used = any(re.search(r"(?<![\w$])" + re.escape(name) + r"(?![\w$])", a) for i in parser.instructions for a in i.args)
            if not used:
                continue
            mapping = spec.get("global_symbols", {}).get(name)
            if mapping is None:
                raise CompileError(f"Referenced global {name} ({size} bytes) lacks a captured global_symbols mapping")
            if isinstance(mapping, int):
                parser.symbols[name] = mapping
                continue
            base = int(mapping.get("virtual_base", mapping.get("address", -1)))
            if base < 0 or int(mapping["bytes"]) < size:
                raise CompileError(f"Invalid original global span for {name}")
            parser.symbols[name] = base
            kind = {"arena": 0, "weights": 1, "constants": 2}[mapping.get("buffer", "constants")]
            offset = int(mapping["arena_offset"])
            condition = f"address>={base}ul && address-{base}ul<={size}ul && n<={size}ul-(address-{base}ul)"
            self.global_load.append(f"if({condition})return rr_buffer_load(c,{kind},{offset}ul+address-{base}ul,n);")
            self.global_store.append(f"if({condition}){{rr_buffer_store(c,{kind},{offset}ul+address-{base}ul,value,n);return;}}")

    def raw(self, token):
        token = token.strip()
        if token in self.reads:
            return self.reads[token]
        special = {"%laneid": "rr_lane", "%warpid": "(rr_thread/32u)", "%nwarpid": "((rr_threads+31u)/32u)", "WARP_SZ": "32u"}
        for prefix, value in (("tid", "rr_tid"), ("ntid", "rr_block"), ("ctaid", "rr_cta"), ("nctaid", "rr_grid")):
            for axis in "xyz":
                special[f"%{prefix}.{axis}"] = f"{value}.{axis}"
        if token in special:
            return special[token]
        if token in self.p.registers:
            return f"rr_registers[{self.register_slots[token]}u*rr_threads+rr_thread]"
        if token in self.p.symbols:
            return f"{self.p.symbols[token]}ul"
        if token.startswith("!"):
            return f"(!{self.raw(token[1:])})"
        if re.fullmatch(r"0[fF][0-9a-fA-F]{8}", token):
            return f"0x{token[2:]}u"
        if re.fullmatch(r"0[dD][0-9a-fA-F]{16}", token):
            return f"0x{token[2:]}ul"
        try:
            integer = re.sub(r"[uUlL]+$", "", token)
            return f"{int(integer, 16 if integer.lstrip('-+').lower().startswith('0x') else 10) & 0xffffffffffffffff}ul"
        except ValueError:
            raise CompileError(f"Unresolved operand {token!r}") from None

    def value(self, token, dtype, ftz=False):
        raw = self.raw(token)
        types = {"f32": f"as_type<float>(uint({raw}))", "f16": f"float(as_type<half>(ushort({raw})))", "f16x2": f"float2(as_type<half2>(uint({raw})))", "s8": f"int(as_type<char>(uchar({raw})))", "s16": f"int(as_type<short>(ushort({raw})))", "s32": f"as_type<int>(uint({raw}))", "s64": f"as_type<long>(ulong({raw}))", "pred": f"bool({raw})"}
        if dtype == "f64":
            raise CompileError("Runtime f64 arithmetic is unavailable; only source-proven literal conversion is supported")
        value = types.get(dtype, f"{'ulong' if dtype.endswith('64') else 'ushort' if dtype.endswith('16') else 'uchar' if dtype.endswith('8') else 'uint'}({raw})")
        return f"rr_ftz({value})" if ftz and dtype.startswith("f") else value

    def assign(self, target, expression, dtype=None, ftz=False):
        if target == "_":
            return ""
        if target not in self.p.registers or (self.reads and target not in self.writes):
            raise CompileError(f"Unknown destination {target}")
        if ftz and dtype == "f32":
            expression = f"rr_ftz(float({expression}))"
        if dtype == "f32":
            expression = f"as_type<uint>(float({expression}))"
        elif dtype == "f16":
            expression = f"as_type<ushort>(half({expression}))"
        elif dtype == "f16x2":
            expression = f"as_type<uint>(half2({expression}))"
        storage = self.p.registers[target]
        cast = "ulong" if storage in ("b64", "u64", "s64", "f64") else "ushort" if storage.endswith("16") else "bool" if storage == "pred" else "uint"
        return f"{self.writes.get(target, self.raw(target))}={cast}({expression});"

    def address(self, operand):
        operand = operand.strip()[1:-1].strip()
        match = re.fullmatch(r"([%\w$.]+)(?:\s*([+-])\s*([+-]?(?:0x[0-9a-fA-F]+|\d+)))?", operand)
        if not match:
            raise CompileError(f"Invalid address {operand}")
        if match[2]:
            displacement = int(match[3], 0) * (1 if match[2] == "+" else -1)
            return f"({self.raw(match[1])}+{displacement & 0xffffffffffffffff}ul)"
        return self.raw(match[1])

    def registers(self, operand):
        return _split(operand[1:-1]) if operand.startswith("{") else [operand]

    def texture(self, inst):
        parts, a = inst.opcode.split("."), inst.args
        base = parts[0]
        if "2d" not in parts:
            raise CompileError(f"Unsupported view dimensionality at line {inst.line}: {inst.opcode}")
        operand = a[0] if base == "sust" else a[1]
        handle, coordinate = _split(operand[1:-1])
        coords = self.registers(coordinate)
        if len(coords) != 2:
            raise CompileError(f"Expected two view coordinates at line {inst.line}")
        if base in ("tex", "tld4"):
            src, dest = parts[-1], parts[-2]
            xy = ",".join(self.value(x,src) for x in coords)
            component = next(("rgba".index(p) for p in parts if p in ("r","g","b","a")),0) if base == "tld4" else 0
            level = self.value(a[2], "f32") if "level" in parts else "0.0f"
            call = f"rr_texture(rr_c,{self.raw(handle)},float2({xy}),{str(src.startswith('s') or src.startswith('u')).lower()},{str(base=='tld4').lower()},{component},{level})"
            targets = self.registers(a[0])
            return "{float4 rr_sample="+call+";"+"".join(self.assign(t,f"rr_sample[{n}]",dest) for n,t in enumerate(targets))+"}"
        xy = ",".join(self.value(x,"s32") for x in coords)
        width = int(re.search(r"\d+",next(p for p in parts if re.fullmatch(r"[bufs]\d+",p)))[0])//8
        names = self.registers(a[1] if base=="sust" else a[0])
        byte_x = str("b" in parts).lower()
        boundary = 0 if "zero" in parts else 1 if "clamp" in parts else 2 if "trap" in parts else None
        if boundary is None:
            raise CompileError(f"Missing surface boundary mode {inst.opcode}")
        if base=="sust":
            return "".join(f"rr_surface_store(rr_c,{self.raw(handle)},int2({xy}),{byte_x},{n},{width},{len(names)},ulong({self.raw(t)}),{boundary});" for n,t in enumerate(names))
        return "".join(self.assign(t,f"rr_surface_load(rr_c,{self.raw(handle)},int2({xy}),{byte_x},{n},{width},{len(names)},{boundary})") for n,t in enumerate(names))

    def view_header(self):
        lines = [r"""
inline int rr_coordinate(int x,int size,uint mode,thread bool& valid) {
  if(mode==0)return clamp(x,0,size-1);
  if(mode==1){valid=valid&&(x>=0&&x<size);return clamp(x,0,size-1);}
  int t=x%size;if(t<0)t+=size;
  if(mode==2)return t;
  int period=2*size;t=x%period;if(t<0)t+=period;return t<size?t:period-1-t;
}
"""]
        texture_cases, load_cases, store_cases = [], [], []
        formats = {"f16":(2,"float(as_type<half>(ushort(rr_bits)))"), "f32":(4,"as_type<float>(uint(rr_bits))"), "u8":(1,"float(uchar(rr_bits))"), "u16":(2,"float(ushort(rr_bits))"), "u32":(4,"float(uint(rr_bits))"), "s8":(1,"float(as_type<char>(uchar(rr_bits)))"), "s16":(2,"float(as_type<short>(ushort(rr_bits)))"), "s32":(4,"float(as_type<int>(uint(rr_bits)))"), "unorm8":(1,"float(uchar(rr_bits))/255.0f"), "unorm16":(2,"float(ushort(rr_bits))/65535.0f"), "snorm8":(1,"max(float(as_type<char>(uchar(rr_bits)))/127.0f,-1.0f)"), "snorm16":(2,"max(float(as_type<short>(ushort(rr_bits)))/32767.0f,-1.0f)")}
        for index, resource in enumerate(self.spec.get("resources", [])):
            if resource.get("view", {}).get("kind") == "global":
                continue
            shape = resource.get("shape")
            if not shape:
                continue  # A global byte span is not a texture view.
            if isinstance(shape, dict):
                w,h,d,ch = (int(shape.get(k,1)) for k in ("width","height","depth","channels"))
            else:
                if len(shape)!=4:
                    raise CompileError("Resource shape must carry width,height,depth,channels")
                w,h,d,ch = map(int,shape)
            if min(w,h,ch)<=0 or d!=1 or ch>4:
                raise CompileError(f"Unsupported 2D resource shape {shape}")
            storage = resource["storage"]
            if isinstance(storage,str):
                fmt = storage
            elif "dtype" in storage:
                fmt = storage["dtype"]
            else:
                original = {2:("f16",4),5:("f16",2),6:("f16",1),1:("f32",4),8:("f32",2),9:("f32",1),26:("snorm8",4)}
                fmt,original_channels = original[int(storage["cubin_format"])]
                if ch!=original_channels:
                    raise CompileError("Original Cubin channel format differs from resource shape")
            if fmt not in formats:
                raise CompileError(f"Unsupported original channel storage {fmt}")
            width, expression = formats[fmt]
            details = {} if isinstance(storage,str) else storage
            stride = int(details.get("pixel_stride",width*ch))
            pitch = int(details.get("row_pitch",resource.get("row_pitch",w*stride)))
            if pitch<w*stride or stride<ch*width or int(resource["bytes"])<(h-1)*pitch+w*stride:
                raise CompileError(f"Resource descriptor extent/pitch does not contain its shape: {index}")
            view = resource["view"]
            if "mode" in view:
                if view["mode"] not in (0,2):
                    raise CompileError(f"Original sampler mode {view['mode']} is unsupported by CUDA backend")
                address = "clamp"
                linear = "linear" if view["mode"]==0 else "point"
                normalized = True
            else:
                address = view["address_mode"]
                linear = view["filter"]
                normalized = bool(view["normalized_coordinates"])
            modes = address if isinstance(address,list) else [address,address]
            if len(modes)!=2 or any(m not in ("clamp","border","wrap","mirror") for m in modes):
                raise CompileError(f"Unsupported texture address mode {address}")
            mx,my = ({"clamp":0,"border":1,"wrap":2,"mirror":3}[m] for m in modes)
            if view.get("channel_read_mode") == "normalized" and fmt in ("u8","u16","s8","s16"):
                expression = formats[("unorm" if fmt[0]=="u" else "snorm")+fmt[1:]][1]
            if linear not in ("point","linear"):
                raise CompileError(f"Unsupported texture filter {linear}")
            row = f"c.resources+{5*index}"
            lines.append(f"inline float4 rr_point_{index}(thread rr_context& c,int2 position){{bool valid=true;int x=rr_coordinate(position.x,{w},{mx},valid),y=rr_coordinate(position.y,{h},{my},valid);if(!valid)return float4(0);const device ulong* r={row};float4 result=float4(0,0,0,1);for(uint k=0;k<{ch};++k){{ulong rr_bits=rr_buffer_load(c,uint(r[4]),r[2]+ulong(y)*{pitch}ul+ulong(x)*{stride}ul+k*{width}ul,{width});result[k]={expression};}}return result;}}")
            point = f"rr_point_{index}"
            tex = f"if(handle==c.resources[{5*index}]){{if(level!=0.0f){{rr_fault(c,4);return float4(NAN);}}float2 p=coordinate;if(!integer) p*=float2({w if normalized else 1},{h if normalized else 1});"
            tex += f"if(gather){{int2 q=int2(floor(p-0.5f));return float4({point}(c,q+int2(0,1))[component],{point}(c,q+int2(1,1))[component],{point}(c,q+int2(1,0))[component],{point}(c,q)[component]);}}"
            if linear=="linear":
                tex += f"if(!integer){{p-=0.5f;int2 q=int2(floor(p));float2 f=rint((p-float2(q))*256.0f)/256.0f;return mix(mix({point}(c,q),{point}(c,q+int2(1,0)),f.x),mix({point}(c,q+int2(0,1)),{point}(c,q+int2(1,1)),f.x),f.y);}}"
            tex += f"return {point}(c,int2(floor(p)));}}"
            texture_cases.append(tex)
            common = f"if(handle==c.resources[{5*index}]){{const device ulong* r={row};uint size=byte_x?n:{width};long x=byte_x?long(position.x):long(position.x)*{stride};long y=position.y;bool valid=x>=0&&x+long(count*size)<={w*stride}&&y>=0&&y<{h};if(!valid){{if(boundary==2)rr_fault(c,16);if(boundary!=1)"
            load = common+f"return 0ul;x=clamp(x,0l,{w*stride}l-long(count*size));y=clamp(y,0l,{h-1}l);}}x+=element*size;ulong rr_bits=rr_buffer_load(c,uint(r[4]),r[2]+ulong(y)*{pitch}ul+ulong(x),size);"
            load_cases.append(load+f"if(!byte_x)return as_type<uint>(float({expression}));return rr_bits;}}")
            encode = {"f16":"rr_half_bits(as_type<float>(uint(value)))", "f32":"uint(value)", "snorm8":"uchar(char(rint(clamp(as_type<float>(uint(value)),-1.0f,1.0f)*127.0f)))", "snorm16":"ushort(short(rint(clamp(as_type<float>(uint(value)),-1.0f,1.0f)*32767.0f)))", "unorm8":"uchar(rint(clamp(as_type<float>(uint(value)),0.0f,1.0f)*255.0f))", "unorm16":"ushort(rint(clamp(as_type<float>(uint(value)),0.0f,1.0f)*65535.0f))"}.get(fmt)
            if encode is None:
                encode = f"{'char' if fmt=='s8' else 'short' if fmt=='s16' else 'int' if fmt=='s32' else 'uchar' if fmt=='u8' else 'ushort' if fmt=='u16' else 'uint'}(as_type<float>(uint(value)))"
            store = f"if(handle==c.resources[{5*index}]){{if(!byte_x&&element>={ch}u)return;const device ulong* r={row};uint size=byte_x?n:{width};long x=position.x,y=position.y;bool valid=byte_x?(x>=0&&x+long(count*size)<={w*stride}):(x>=0&&x<{w});valid=valid&&y>=0&&y<{h};if(!valid){{if(boundary==2)rr_fault(c,16);if(boundary!=1)return;x=byte_x?clamp(x,0l,{w*stride}l-long(count*size)):clamp(x,0l,{w-1}l);y=clamp(y,0l,{h-1}l);}}if(!byte_x){{x*={stride};value=ulong({encode});}}x+=element*size;rr_buffer_store(c,uint(r[4]),r[2]+ulong(y)*{pitch}ul+ulong(x),value,size);return;}}"
            store_cases.append(store)
        lines.append("inline float4 rr_texture(thread rr_context& c,ulong handle,float2 coordinate,bool integer,bool gather,uint component,float level){"+"".join(texture_cases)+"rr_fault(c,4);return float4(NAN);}")
        lines.append("inline ulong rr_surface_load(thread rr_context& c,ulong handle,int2 position,bool byte_x,uint element,uint n,uint count,uint boundary){"+"".join(load_cases)+"rr_fault(c,4);return ~0ul;}")
        lines.append("inline void rr_surface_store(thread rr_context& c,ulong handle,int2 position,bool byte_x,uint element,uint n,uint count,ulong value,uint boundary){"+"".join(store_cases)+"rr_fault(c,4);}")
        return "\n".join(lines)

    def instruction(self, inst):
        op, a = inst.opcode, inst.args
        parts = op.split(".")
        base, dtype = parts[0], parts[-1]
        ftz = "ftz" in parts
        if base in ("bra", "ret", "bar", "mma", "shfl", "movmatrix", "atom", "membar"):
            raise CompileError("Control/collective instruction must be lowered by the uniform scheduler")
        if base in ("trap", "brkpt"):
            return "rr_fault(rr_c,16);rr_pc=0xffffffffu;"
        if base == "mov":
            if a[0].startswith("{"):
                names = self.registers(a[0]); width = int(dtype[1:]) // len(names)
                return "".join(self.assign(t, f"({self.raw(a[1])}>>{n*width})&{(1<<width)-1}ul") for n,t in enumerate(names))
            if a[1].startswith("{"):
                names = self.registers(a[1]); width = int(dtype[1:]) // len(names)
                return self.assign(a[0], "|".join(f"((ulong({self.raw(t)})&{(1<<width)-1}ul)<<{n*width})" for n,t in enumerate(names)))
            return self.assign(a[0], self.raw(a[1]))
        if base == "cvta":
            return self.assign(a[0], self.raw(a[1]))
        if base in ("ld", "st"):
            width = int(re.search(r"\d+$", dtype)[0])
            group = re.search(r"\.v([24])\.", op)
            count = int(group[1]) if group else 1
            space = 1 if "param" in parts else 2 if "shared" in parts else 0 if "global" in parts or "const" in parts else None
            if space is None:
                raise CompileError(f"Unsupported memory space {op}")
            address = self.address(a[1] if base == "ld" else a[0])
            names = self.registers(a[0] if base == "ld" else a[1])
            if len(names) != count:
                raise CompileError(f"Vector operand mismatch {op}")
            lines = []
            for n,target in enumerate(names):
                current = f"({address}+{n*width//8}ul)"
                if base == "ld":
                    value = f"rr_load(rr_c,{current},{width//8},{space})"
                    if dtype.startswith("s"):
                        value = f"as_type<{'char' if width==8 else 'short' if width==16 else 'int' if width==32 else 'long'}>({'uchar' if width==8 else 'ushort' if width==16 else 'uint' if width==32 else 'ulong'}({value}))"
                    lines.append(self.assign(target,value))
                else:
                    lines.append(f"rr_store(rr_c,{current},ulong({self.raw(target)}),{width//8},{space});")
            return "".join(lines)
        if base in ("tex", "tld4", "suld", "sust"):
            return self.texture(inst)
        if base == "cvt":
            dest, src = parts[-2:]
            if dest == "f64" and src == "f32":
                return self.assign(a[0], f"rr_f32_to_f64({self.value(a[1], src, ftz)})")
            if dest == "f32" and src == "f64":
                return self.assign(a[0], f"rr_f64_to_f32(ulong({self.raw(a[1])}))", "f32", ftz)
            if dest == "e4m3x2" and src == "f16x2":
                return self.assign(a[0], f"rr_fp8x2(uint({self.raw(a[1])}),{str('relu' in parts).lower()})")
            if dest == "f16x2" and src == "e4m3x2":
                return self.assign(a[0], f"rr_fp8_decode2(uint({self.raw(a[1])}),{str('relu' in parts).lower()})")
            if src == "f64":
                literal = a[1] if a[1].startswith("0d") else self.folded.get(a[1])
                if literal is None or dest != "f16" or "rn" not in parts:
                    raise CompileError(f"Cannot fold exact f64 conversion at line {inst.line}")
                return self.assign(a[0],f"{_half_literal(literal)}u")
            v = self.value(a[1], src, ftz)
            rounding = next((p for p in parts if p in ("rzi","rni","rmi","rpi","rz","rm","rp")), None)
            if rounding:
                fn = {"rzi":"trunc","rni":"rint","rmi":"floor","rpi":"ceil","rz":"trunc","rm":"floor","rp":"ceil"}[rounding]
                if src.startswith("f") and (not dest.startswith("f") or src == dest):
                    v = f"{fn}({v})"
                elif dest == "f16" and src == "f32" and rounding == "rz":
                    return self.assign(a[0],f"rr_half_bits_rz({v})")
                elif dest.startswith("f"):
                    raise CompileError(f"Unimplemented directed floating conversion {op}")
            if dest == "f16x2":
                v = f"float2({self.value(a[2],src,ftz)},{v})"
            if "sat" in parts:
                if dest.startswith("f"):
                    v = f"(isnan({v})?0.0f:clamp({v},0.0f,1.0f))"
                else:
                    bits = int(dest[1:]); lower = -(1<<(bits-1)) if dest.startswith("s") else 0; upper = (1<<(bits-1))-1 if dest.startswith("s") else (1<<bits)-1
                    v = f"clamp({v},float({lower}),float({upper}))"
            if not dest.startswith("f"):
                typ = {"s8":"char","s16":"short","s32":"int","s64":"long","u8":"uchar","u16":"ushort","u32":"uint","u64":"ulong"}.get(dest)
                if typ is None:
                    raise CompileError(f"Unsupported conversion {op}")
                if src.startswith("f"):
                    v = f"rr_float_integer(float({v}),{int(dest[1:])},{str(dest.startswith('s')).lower()})"
                else:
                    v = f"{typ}({v})"
            return self.assign(a[0],v,dest if dest.startswith("f") else None,ftz)
        if base in ("set", "setp"):
            cmp = parts[1]; src = parts[-1]
            x,y = self.value(a[1],src,ftz),self.value(a[2],src,ftz)
            relation = {"eq":"==","ne":"!=","lt":"<","le":"<=","gt":">","ge":">=","lo":"<","ls":"<=","hi":">","hs":">="}.get(cmp.rstrip("u"))
            if cmp == "num":
                v = f"(!isnan({x})&&!isnan({y}))"
            elif cmp == "nan":
                v = f"(isnan({x})||isnan({y}))"
            elif relation:
                v = f"({x}{relation}{y})"
                if src.startswith("f"):
                    nan = f"(isnan({x})|isnan({y}))"
                    v = f"({v}|{nan})" if cmp.endswith("u") else f"({v}&!{nan})"
            else:
                raise CompileError(f"Unknown comparison {op}")
            boolean = next((p for p in parts[2:-1] if p in ("and","or","xor")),None)
            names = a[0].split("|")
            complement = f"(!({v}))"
            if boolean:
                c = self.raw(a[3]); operator = {"and":"&","or":"|","xor":"^"}[boolean]
                complement = f"({complement}{operator}bool({c}))"
                v = f"({v}{operator}bool({c}))"
            if base == "setp":
                return self.assign(names[0],v)+(self.assign(names[1],complement) if len(names)>1 else "")
            dest = parts[-2]
            if src == "f16x2":
                return "{bool2 rr_match="+v+";"+self.assign(names[0],"(rr_match.x?65535u:0u)|(rr_match.y?0xffff0000u:0u)")+"}"
            return self.assign(names[0],f"({v}?{'1.0f' if dest.startswith('f') else '~0u'}:0)",dest if dest.startswith("f") else None)
        if base == "selp":
            return self.assign(a[0],f"({self.raw(a[3])}?{self.raw(a[1])}:{self.raw(a[2])})")
        if dtype in ("f16", "f16x2") and base in ("abs", "neg"):
            mask = "0x7fff7fffu" if dtype == "f16x2" else "0x7fffu"
            sign = "0x80008000u" if dtype == "f16x2" else "0x8000u"
            return self.assign(a[0],f"(uint({self.raw(a[1])}){'&'+mask if base=='abs' else '^'+sign})")
        if dtype in ("f16", "f16x2") and base in ("add","sub","mul","fma"):
            helper = "rr_half2_" if dtype == "f16x2" else "rr_half_"
            return self.assign(a[0], f"{helper}{base}({','.join('uint('+self.raw(v)+')' for v in a[1:])})")
        if dtype == "f64" and base in ("max", "div"):
            return self.assign(a[0], f"rr_f64_{base}(ulong({self.raw(a[1])}),ulong({self.raw(a[2])}))")
        values = [self.value(v,dtype,ftz) for v in a[1:]]
        x = values[0] if values else ""
        if base in ("add","sub","mul","mad") and not dtype.startswith("f"):
            signed = dtype.startswith("s"); bits = int(dtype[1:])
            unsigned = "ulong" if bits == 64 else "uint"
            uv = [f"{unsigned}({self.raw(v)})" for v in a[1:]]
            if "wide" in parts:
                cast = "long" if signed else "ulong"
                uv = [f"{cast}({self.value(v,dtype)})" for v in a[1:]]
            if "hi" in parts:
                v = f"mulhi({values[0]},{values[1]})"
                if base == "mad":
                    v = f"({v}+{values[2]})"
            elif base == "mad":
                v = f"({uv[0]}*{uv[1]}+{uv[2]})"
            else:
                v = f"({uv[0]}{ {'add':'+','sub':'-','mul':'*'}[base]}{uv[1]})"
        elif base in ("add","sub","mul","div","rem","and","or","xor","shl","shr"):
            operator = {"add":"+","sub":"-","mul":"*","div":"/","rem":"%","and":"&","or":"|","xor":"^","shl":"<<","shr":">>"}[base]
            if base in ("shl","shr"):
                bits = int(dtype[1:]); shift = f"min(uint({self.raw(a[2])}),{bits}u)"
                v = f"({shift}>={bits}u?{'({} < 0 ? -1 : 0)'.format(x) if base=='shr' and dtype.startswith('s') else '0'}:({x}{operator}{shift}))"
            else:
                v = f"rr_add_rz({x},{values[1]})" if base == "add" and "rz" in parts else f"({x}{operator}{values[1]})"
        elif base == "fma":
            v = f"fma({','.join(values)})"
        elif base == "mad":
            v = f"({values[0]}*{values[1]}+{values[2]})"
        elif base in ("min","max"):
            v = f"{base}({values[0]},{values[1]})"
        elif base == "not":
            v = f"(!{x})" if dtype == "pred" else f"(~{x})"
        elif base == "neg":
            v = f"(-{x})" if dtype.startswith("f") else f"(0ul-ulong({self.raw(a[1])}))"
        elif base == "abs":
            v = f"abs({x})"
        elif base in ("ex2","lg2","rcp","rsqrt","sqrt","tanh","sin","cos"):
            code = {"ex2":0,"lg2":1,"rcp":2,"rsqrt":3,"sqrt":4,"tanh":5,"sin":6,"cos":7}[base] + (256 if ftz else 0)
            v = f"rr_sfu({x},{code}u)"
        elif base == "bfi":
            v = f"rr_bfi({','.join('uint('+self.raw(v)+')' for v in a[1:])})"
        else:
            raise CompileError(f"Unsupported opcode {op} at line {inst.line}: {inst.text}")
        if "sat" in parts:
            v = f"clamp({v},0.0f,1.0f)"
        return self.assign(a[0],v,dtype if dtype.startswith("f") else None,ftz)


def _collective_code(e, inst):
    """One opcode handler; participation and PC advancement belong to the scheduler."""
    base, a = inst.opcode.split('.')[0], inst.args
    if base == 'mma':
        dest, aa, bb, cc = map(e.registers, a)
        if tuple(map(len, (dest, aa, bb, cc))) != (2, 4, 2, 2):
            raise CompileError(f'MMA fragment operand sizes at line {inst.line}')
        args = ','.join(f"uint{n}({','.join('uint('+e.raw(t)+')' for t in frag)})"
                        for n, frag in ((4, aa), (2, bb), (2, cc)))
        return f'uint2 rr_result=rr_mma({args},rr_lane);if(rr_active){{' + ''.join(
            e.assign(t, f'rr_result[{n}]') for n, t in enumerate(dest)) + '}'
    if base == 'movmatrix':
        return f'uint rr_result=rr_movmatrix(uint({e.raw(a[1])}),rr_lane);if(rr_active){{' + e.assign(a[0], 'rr_result') + '}'
    if base == 'vote':
        return f'uint rr_result=uint(simd_vote::vote_t(simd_ballot(rr_active && bool({e.raw(a[1])}) && bool(uint({e.raw(a[2])}) & (1u<<rr_lane)))));if(rr_active){{' + e.assign(a[0], 'rr_result') + '}'
    if base == 'shfl':
        mode = {'up': 0, 'down': 1, 'bfly': 2, 'idx': 3}[inst.opcode.split('.')[2]]
        dest = a[0].split('|')
        return f'uint2 rr_result=rr_shuffle(uint({e.raw(a[1])}),rr_lane,uint({e.raw(a[2])}),uint({e.raw(a[3])}),uint({e.raw(a[4])}),{mode}u);if(rr_active){{if(!(rr_result.y&2u))rr_fault(rr_c,8);' + e.assign(dest[0], 'rr_result.x') + (e.assign(dest[1], 'rr_result.y&1u') if len(dest) > 1 else '') + '}'
    if base == 'membar':
        return 'threadgroup_barrier(mem_flags::mem_device|mem_flags::mem_threadgroup);'
    if base == 'atom':
        width = 8 if inst.opcode.endswith('u64') else 4
        space = 2 if '.shared.' in inst.opcode else 0
        address = e.address(a[1])
        value = f'old+ulong({e.raw(a[2])})'
        if inst.opcode.endswith('f32'):
            value = f"as_type<float>(uint(old))+{e.value(a[2], 'f32')}"
            if space == 0:
                value = f"rr_ftz(rr_ftz(as_type<float>(uint(old)))+{e.value(a[2], 'f32', True)})"
            value = f'ulong(as_type<uint>({value}))'
        return f'for(uint owner=0;owner<rr_threads;++owner){{if(rr_active&&rr_thread==owner){{ulong old=rr_load(rr_c,{address},{width},{space});rr_store(rr_c,{address},{value},{width},{space});' + e.assign(a[0], 'old') + '}threadgroup_barrier(mem_flags::mem_device|mem_flags::mem_threadgroup);}'
    raise CompileError(f'Unsupported collective {inst.opcode}')


def _compile_kernel(source, entry, dispatch_spec, phase_cuts=None):
    """Compile exact PTX to immutable instruction records and a bounded GPU executor.

    Only decoding/constant folding occurs here. Every dynamic instruction, branch,
    collective and memory operation runs on the GPU, in the original CUDA geometry.
    A record is eight uint64 header words followed by (kind, value, offset) operands.
    Operand kinds are literal=0, register=1, CUDA builtin=2. Device register slots
    retain bits; the shared-memory and collective scheduler contracts are unchanged.
    """
    p = Parser(source, entry)
    if dispatch_spec.get('entry', entry) != entry:
        raise CompileError('Dispatch entry differs from requested entry')
    block = tuple(int(x) for x in dispatch_spec['cuda_block'])
    grid = tuple(int(x) for x in dispatch_spec['cuda_grid'])
    if len(block) != 3 or len(grid) != 3 or min(*block, *grid) <= 0:
        raise CompileError('CUDA geometry must have three positive dimensions')
    threads = block[0] * block[1] * block[2]
    if threads > 1024:
        raise CompileError('CUDA block exceeds 1024 threads')
    original = Emitter(p, dispatch_spec)
    instructions = p.instructions
    resume = phase_cuts is not None
    cuts = []
    if resume:
        covered = set()
        for start, end in phase_cuts:
            if not isinstance(start, int) or not isinstance(end, int) or not 0 <= start < end <= len(instructions):
                raise CompileError('Phase cut must be a nonempty source-PC interval')
            pcs = set(range(start, end))
            if covered & pcs or any(instructions[pc].opcode.split('.')[0] != 'mma' for pc in pcs):
                raise CompileError('Phase cuts must be disjoint contiguous MMA instructions')
            covered.update(pcs)
            cuts.append((start, end))
        if covered != {pc for pc, inst in enumerate(instructions) if inst.opcode.split('.')[0] == 'mma'}:
            raise CompileError('Phase cuts must cover every original MMA instruction')
        cuts.sort()
    endpc, no_predicate = 0xffffffff, 0xffffffffffffffff
    builtins = ['%laneid', '%warpid', '%nwarpid', 'WARP_SZ'] + [
        f'%{prefix}.{axis}' for prefix in ('tid', 'ntid', 'ctaid', 'nctaid') for axis in 'xyz']
    variants, records, sites = {}, [], []
    for index, inst in enumerate(instructions):
        if not _supported(inst.opcode):
            raise CompileError(f'{entry}:{inst.line}: Unsupported opcode/modifier tuple {inst.opcode}')
        base = inst.opcode.split('.')[0]
        kind = (1 if base in ('mma', 'shfl', 'movmatrix', 'vote') else 2 if base == 'bar'
                else 3 if base in ('atom', 'membar') else 4 if base == 'bra'
                else 5 if base == 'ret' else 6 if base in ('trap', 'brkpt') else 0)
        if 1 <= kind <= 3:
            sites.append({'instruction': index, 'line': inst.line, 'opcode': inst.opcode,
                          'kind': ('warp', 'cta_barrier', 'ordered_memory')[kind - 1], 'predicate': inst.predicate})
        predicate = no_predicate
        if inst.predicate:
            name = inst.predicate.lstrip('!')
            if p.registers.get(name) != 'pred':
                raise CompileError(f'Unknown predicate {inst.predicate}')
            predicate = original.register_slots[name] | ((1 << 63) if inst.predicate.startswith('!') else 0)
        operands, registers, writable = [], {}, set()

        def operand(token, offset=0):
            token = token.strip()
            if token == '_':
                return token
            if token.startswith('!'):
                return '!' + operand(token[1:])
            name = f'o{len(operands)}'
            if token in p.registers:
                tag, value, dtype = 1, original.register_slots[token], p.registers[token]
                writable.add(name)
            elif token in builtins:
                tag, value, dtype = 2, builtins.index(token), 'b64'
            else:
                tag, dtype = 0, 'b64'
                if token in p.symbols:
                    value = p.symbols[token]
                elif re.fullmatch(r'0[fFdD][0-9a-fA-F]+', token):
                    value = int(token[2:], 16)
                else:
                    literal = re.sub(r'[uUlL]+$', '', token)
                    try:
                        value = int(literal, 16 if literal.lstrip('-+').lower().startswith('0x') else 10)
                    except ValueError:
                        raise CompileError(f'{entry}:{inst.line}: Unresolved operand {token!r}') from None
            operands.append((tag, value & no_predicate, offset & no_predicate))
            registers[name] = dtype
            return name

        def canonical(argument):
            argument = argument.strip()
            if argument.startswith('{'):
                return '{' + ','.join(canonical(x) for x in _split(argument[1:-1])) + '}'
            if argument.startswith('['):
                parts = _split(argument[1:-1])
                if len(parts) > 1:
                    return '[' + ','.join(canonical(x) for x in parts) + ']'
                match = re.fullmatch(r'([%\w$.]+)(?:\s*([+-])\s*([+-]?(?:0x[0-9a-fA-F]+|\d+)))?', parts[0])
                if not match:
                    raise CompileError(f'Invalid address {argument}')
                displacement = int(match[3], 0) * (1 if match[2] == '+' else -1) if match[2] else 0
                return '[' + operand(match[1], displacement) + ']'
            if '|' in argument:
                return '|'.join(canonical(x) for x in argument.split('|'))
            return operand(argument)

        branch, mask = endpc, endpc
        opcode, arguments = inst.opcode, inst.args
        if base == 'bra':
            if len(arguments) != 1 or arguments[0] not in p.labels:
                raise CompileError(f'Unknown branch label at line {inst.line}')
            branch = p.labels[arguments[0]]
            if branch == len(instructions):
                branch = endpc
            arguments = ()
        if opcode == 'mov.f64':
            opcode = 'mov.b64'
        elif opcode == 'cvt.rn.f16.f64':
            literal = arguments[1] if arguments[1].startswith('0d') else original.folded.get(arguments[1])
            if literal is None:
                raise CompileError(f'Cannot fold exact f64 conversion at line {inst.line}')
            opcode, arguments = 'mov.b16', (arguments[0], str(_half_literal(literal)))
        args = tuple(canonical(a) for a in arguments)
        if base in ('shfl', 'vote'):
            mask = int(args[-1][1:])
        if kind == 2 and len(args) not in (1, 2):
            raise CompileError(f'Unsupported barrier at line {inst.line}')
        fake = SimpleNamespace(registers=registers, instructions=[], globals=[], symbols={})
        emitter = Emitter(fake, dispatch_spec)
        emitter.reads = {name: f'rr_values[{i}]' for i, name in enumerate(registers)}
        emitter.writes = {name: f'rr_registers[rr_dest[{i}]*rr_threads+rr_thread]'
                          for i, name in enumerate(registers) if name in writable}
        normalized = Instruction(opcode, args, None, inst.line, inst.end_line, inst.offset, inst.end_offset, inst.text)
        try:
            code = (emitter.instruction(normalized) if kind == 0 else
                    _collective_code(emitter, normalized) if kind in (1, 3) else '')
        except (CompileError, ValueError, KeyError) as error:
            raise CompileError(f'{entry}:{inst.line}: {inst.opcode}: {error}') from error
        key = kind, code
        if key not in variants:
            variants[key] = len(variants)
        records.append(([variants[key], kind, predicate, len(operands), branch, mask,
                         int(base in ('mma', 'movmatrix')), int(resume and base == 'mma')], operands))
    max_operands = max(1, max(len(operands) for _, operands in records))
    stride = 8 + max_operands * 3
    words = []
    for record, operands in records:
        words.extend(record)
        words.extend(v for triple in operands for v in triple)
        words.extend([0] * ((max_operands - len(operands)) * 3))
    program = struct.pack(f'<{len(words)}Q', *words)
    header = _CONTEXT.replace('RR_SHARED_BYTES', f'{p.shared_bytes}ul').replace(
        'RR_GLOBAL_LOAD', ''.join(original.global_load)).replace('RR_GLOBAL_STORE', ''.join(original.global_store))
    header += '\n' + original.view_header()
    header += f'\ninline uint rr_kind(const device ulong* program,uint pc){{return pc==0xffffffffu?0u:uint(program[ulong(pc)*{stride}ul+1]);}}\n'
    header += f'inline bool rr_full_warp(const device ulong* program,uint pc){{return bool(program[ulong(pc)*{stride}ul+6]);}}\n'
    header += r'''
inline ulong rr_operand(const device ulong* argument,device volatile ulong* registers,
                        thread const ulong* builtins,uint threads,uint thread_index){
  ulong value=argument[0]==0?argument[1]:argument[0]==1?
      registers[argument[1]*threads+thread_index]:builtins[argument[1]];
  return value+argument[2];
}
'''
    header = header.replace('for(', '\n#pragma clang loop unroll(disable)\nfor(')
    lines, handler_lines = [], {}

    def add(text):
        lines.extend(re.sub(r'\b(for|while)\(', r'\n#pragma clang loop unroll(disable)\n\1(', text).splitlines())

    register_pointer = ('(device volatile ulong*)registers_out' if resume else
                        '(device volatile ulong*)(arena_out+((ulong(launch[6])+7ul)&~7ul))')
    cta_begin = ('uint rr_cta_linear=resume_info[1];{' if resume else
                 'for(uint rr_cta_linear=0;rr_cta_linear<rr_grid.x*rr_grid.y*rr_grid.z;++rr_cta_linear){')
    simd_guard = ('if(threads_per_simdgroup!=32){if(rr_thread==0)rr_fault(rr_c,32);}' if resume else
                  'if(threads_per_simdgroup!=32){if(rr_thread==0)rr_fault(rr_c,32);return;}')
    add(f'''uint rr_thread=thread_position_in_threadgroup.x;
uint rr_lane=thread_index_in_simdgroup;
uint rr_threads={threads}u;
device volatile ulong* rr_registers={register_pointer};
uint3 rr_grid=uint3(launch[0],launch[1],launch[2]);
uint3 rr_block=uint3(launch[3],launch[4],launch[5]);
uint3 rr_tid=uint3(rr_thread%rr_block.x,(rr_thread/rr_block.x)%rr_block.y,rr_thread/(rr_block.x*rr_block.y));
threadgroup uchar rr_shared[{max(1,p.shared_bytes)}];
threadgroup atomic_uint rr_defined[{max(1,(p.shared_bytes+31)//32)}];
threadgroup uint rr_pcs[{threads}],rr_members[{threads}],rr_barids[{threads}],rr_counts[{threads}],rr_next[{threads}],rr_run[{threads}];
threadgroup uint rr_selected,rr_selected_kind,rr_finish;
rr_context rr_c={{arena_out,weights,constants,params,resource_table,launch,diagnostics,rr_shared,rr_defined,0,0}};
for(uint i=rr_thread;i<launch[6];i+=rr_threads)arena_out[i]=arena_in[i];
if(rr_thread==0){{diagnostics[0]=0;diagnostics[1]=0;diagnostics[2]=0;diagnostics[3]=0;}}
threadgroup_barrier(mem_flags::mem_device|mem_flags::mem_threadgroup);
{simd_guard}
{cta_begin}
rr_c.cta=rr_cta_linear;
uint3 rr_cta=uint3(rr_cta_linear%rr_grid.x,(rr_cta_linear/rr_grid.x)%rr_grid.y,rr_cta_linear/(rr_grid.x*rr_grid.y));
ulong rr_builtins[16]={{rr_lane,rr_thread/32u,(rr_threads+31u)/32u,32u,
rr_tid.x,rr_tid.y,rr_tid.z,rr_block.x,rr_block.y,rr_block.z,
rr_cta.x,rr_cta.y,rr_cta.z,rr_grid.x,rr_grid.y,rr_grid.z}};
ulong rr_values[{max_operands}];uint rr_dest[{max_operands}];
for(uint i=rr_thread;i<{max(1,(p.shared_bytes+31)//32) if resume else (p.shared_bytes+31)//32}u;i+=rr_threads)atomic_store_explicit(rr_defined+i,{('resume_info[0]?0u:defined_in[i]' if resume else '0u')},memory_order_relaxed);
threadgroup_barrier(mem_flags::mem_threadgroup);
''')
    if resume:
        add(f'''for(uint i=rr_thread;i<{max(1,p.shared_bytes)}u;i+=rr_threads)rr_shared[i]=resume_info[0]?0:shared_in[i];
if(rr_thread==0 && (resume_info[0]>1u || rr_cta_linear>=rr_grid.x*rr_grid.y*rr_grid.z))rr_fault(rr_c,16);
if(resume_info[0]){{''')
    first = 0
    for dtype, group in groupby(p.registers.values()):
        count = sum(1 for _ in group)
        poison = ('0ul' if dtype == 'pred' else '0xdeadbeefdeadbeeful' if dtype.endswith('64')
                  else '0x7e01ul' if dtype.endswith('16') else '0xdeadbeeful')
        add(f'for(uint slot={first}u;slot<{first+count}u;++slot)rr_registers[slot*rr_threads+rr_thread]={poison};')
        first += count
    if resume:
        add(f'''}}else{{for(uint slot=0;slot<{len(p.registers)}u;++slot)rr_registers[slot*rr_threads+rr_thread]=registers_in[slot*rr_threads+rr_thread];}}
rr_run[rr_thread]=resume_info[0]?0u:control_in[rr_thread*2u+1u];
uint rr_pc=resume_info[0]?0u:control_in[rr_thread*2u];
threadgroup_barrier(mem_flags::mem_device|mem_flags::mem_threadgroup);
if(diagnostics[0])rr_pc=0xffffffffu;
uint rr_event=2u;
while(true){{''')
    else:
        add('uint rr_pc=0;\nwhile(true){')
    control_line = len(lines) + 1
    add(f'''while(rr_pc!=0xffffffffu){{
  if(rr_pc>={len(instructions)}u){{rr_pc=0xffffffffu;break;}}
  rr_c.instruction=rr_pc;
  const device ulong* rr_record=program+ulong(rr_pc)*{stride}ul;
  ulong pred=rr_record[2];
  if(pred!=~0ul && bool(rr_registers[(pred&0x7ffffffffffffffful)*rr_threads+rr_thread])==bool(pred>>63)){{++rr_pc;continue;}}
  uint kind=uint(rr_record[1]);
  if(kind==1||kind==2||kind==3)break;
  if(kind==4){{rr_pc=uint(rr_record[4]);continue;}}
  if(kind==5){{rr_pc=0xffffffffu;break;}}
  if(kind==6){{rr_fault(rr_c,16);rr_pc=0xffffffffu;break;}}
  for(uint i=0;i<uint(rr_record[3]);++i){{
    const device ulong* argument=rr_record+8+3*i;
    rr_values[i]=rr_operand(argument,rr_registers,rr_builtins,rr_threads,rr_thread);
    rr_dest[i]=uint(argument[1]);
  }}
  switch(uint(rr_record[0])){{
''')
    for (kind, code), handler in variants.items():
        if kind != 0:
            continue
        begin = len(lines) + 1
        add(f'case {handler}u:{{{code}break;}}')
        handler_lines[handler] = begin, len(lines)
    add('default:rr_fault(rr_c,16);break;}\n++rr_pc;\n}')
    add(f'''rr_members[rr_thread]=0xffffffffu;rr_barids[rr_thread]=0;rr_counts[rr_thread]=0;
rr_next[rr_thread]=rr_pc+1u=={len(instructions)}u?0xffffffffu:rr_pc+1u;
if(rr_pc!=0xffffffffu){{
  const device ulong* rr_record=program+ulong(rr_pc)*{stride}ul;
  if(rr_record[5]!=0xfffffffful)rr_members[rr_thread]=uint(rr_operand(rr_record+8+3*rr_record[5],rr_registers,rr_builtins,rr_threads,rr_thread));
  if(rr_record[1]==2){{
    rr_barids[rr_thread]=uint(rr_operand(rr_record+8,rr_registers,rr_builtins,rr_threads,rr_thread));
    if(rr_record[3]==2)rr_counts[rr_thread]=uint(rr_operand(rr_record+11,rr_registers,rr_builtins,rr_threads,rr_thread));
  }}
}}
rr_pcs[rr_thread]=rr_pc;
threadgroup_barrier(mem_flags::mem_device|mem_flags::mem_threadgroup);
''')
    add(r"""if(rr_thread==0){
  rr_selected=0xffffffffu;rr_selected_kind=0;rr_finish=1;
  for(uint t=0;t<rr_threads;++t){
    rr_run[t]=0;uint pc=rr_pcs[t];if(pc==0xffffffffu)continue;rr_finish=0;
    uint kind=rr_kind(program,pc);bool ready=true;
    if(kind==1){
      uint mask=rr_members[t],warp=t&~31u;ready=bool(mask&(1u<<(t&31u)));
      for(uint lane=0;lane<32;++lane)if(mask&(1u<<lane)){
        uint other=warp+lane;
        if((other>=rr_threads||rr_pcs[other]==0xffffffffu)&&!rr_full_warp(program,pc))continue;
        if(other>=rr_threads||rr_pcs[other]!=pc||rr_members[other]!=mask)ready=false;
      }
    }else if(kind==2){
      uint count=0,id=rr_barids[t],expected=rr_counts[t];
      if(expected==0){expected=0;for(uint other=0;other<rr_threads;++other)expected+=rr_pcs[other]!=0xffffffffu;}
      if(id>15||expected==0||expected>rr_threads||(rr_counts[t]!=0&&expected%32!=0))ready=false;
      for(uint other=0;other<rr_threads;++other)if(rr_kind(program,rr_pcs[other])==2&&rr_barids[other]==id){
        ++count;if(rr_counts[other]!=rr_counts[t])ready=false;
      }
      ready=ready&&count==expected;
    }
    if(ready&&pc<rr_selected){rr_selected=pc;rr_selected_kind=kind;}
  }
  if(!rr_finish&&rr_selected==0xffffffffu){
    rr_c.instruction=0xffffffffu;for(uint t=0;t<rr_threads;++t)rr_c.instruction=min(rr_c.instruction,rr_pcs[t]);
    rr_fault(rr_c,8);
  }
  if(rr_selected!=0xffffffffu){
    uint id=0;for(uint t=0;t<rr_threads;++t)if(rr_pcs[t]==rr_selected){id=rr_barids[t];break;}
    for(uint t=0;t<rr_threads;++t){
      if(rr_selected_kind==2){rr_run[t]=rr_kind(program,rr_pcs[t])==2&&rr_barids[t]==id;continue;}
      if(rr_pcs[t]!=rr_selected)continue;
      bool ready=true;
      if(rr_selected_kind==1){
        uint mask=rr_members[t],warp=t&~31u;ready=bool(mask&(1u<<(t&31u)));
        for(uint lane=0;lane<32;++lane)if(mask&(1u<<lane)){
          uint other=warp+lane;
          if((other>=rr_threads||rr_pcs[other]==0xffffffffu)&&!rr_full_warp(program,rr_selected))continue;
          if(other>=rr_threads||rr_pcs[other]!=rr_selected||rr_members[other]!=mask)ready=false;
        }
      }
      rr_run[t]=ready;
    }
  }
}
threadgroup_barrier(mem_flags::mem_device|mem_flags::mem_threadgroup);
if(rr_finish||rr_selected==0xffffffffu||diagnostics[0])break;
""")
    if resume:
        cut_guard = '||'.join(f'rr_selected=={start}u' for start, _ in cuts) or 'false'
        add(f'''if(program[ulong(rr_selected)*{stride}ul+7]){{
if({cut_guard})rr_event=1u;
else if(rr_thread==0){{rr_c.instruction=rr_selected;rr_fault(rr_c,16);}}
threadgroup_barrier(mem_flags::mem_device|mem_flags::mem_threadgroup);
break;
}}''')
    add('bool rr_active=bool(rr_run[rr_thread]);')
    add(f'''rr_c.instruction=rr_selected;
if(rr_selected_kind==2){{
  threadgroup_barrier(mem_flags::mem_device|mem_flags::mem_threadgroup);
}}else{{
  const device ulong* rr_record=program+ulong(rr_selected)*{stride}ul;
  for(uint i=0;i<uint(rr_record[3]);++i){{
    const device ulong* argument=rr_record+8+3*i;
    rr_values[i]=rr_operand(argument,rr_registers,rr_builtins,rr_threads,rr_thread);
    rr_dest[i]=uint(argument[1]);
  }}
  switch(uint(rr_record[0])){{
''')
    for (kind, code), handler in variants.items():
        if kind not in (1, 3):
            handler_lines.setdefault(handler, (control_line, len(lines)))
            continue
        begin = len(lines) + 1
        if resume and 'rr_mma(' in code:
            code = 'if(rr_active)rr_fault(rr_c,16);'
        add(f'case {handler}u:{{{code}break;}}')
        handler_lines[handler] = begin, len(lines)
    add('default:if(rr_active)rr_fault(rr_c,8);break;}\n}\nif(rr_active)rr_pc=rr_next[rr_thread];\nthreadgroup_barrier(mem_flags::mem_device|mem_flags::mem_threadgroup);\n}\nthreadgroup_barrier(mem_flags::mem_device|mem_flags::mem_threadgroup);')
    if resume:
        add(f'''control_out[rr_thread*2u]=rr_pc;
control_out[rr_thread*2u+1u]=rr_run[rr_thread];
for(uint i=rr_thread;i<{max(1,p.shared_bytes)}u;i+=rr_threads)shared_out[i]=rr_shared[i];
for(uint i=rr_thread;i<{max(1,(p.shared_bytes+31)//32)}u;i+=rr_threads)defined_out[i]=atomic_load_explicit(rr_defined+i,memory_order_relaxed);
threadgroup_barrier(mem_flags::mem_device|mem_flags::mem_threadgroup);
if(rr_thread==0){{status[0]=diagnostics[0]?3u:rr_event;status[1]=rr_selected;status[2]=rr_cta_linear;status[3]=0u;}}
}}''')
    else:
        add('if(diagnostics[0])break;\n}')
    msl = '\n'.join(lines)
    source_map = [{'instruction': i, 'opcode': inst.opcode, 'ptx_line': inst.line,
                   'ptx_end_line': inst.end_line, 'ptx_offset': inst.offset, 'ptx_end_offset': inst.end_offset,
                   'msl_line': handler_lines[records[i][0][0]][0], 'msl_end_line': handler_lines[records[i][0][0]][1],
                   'handler': records[i][0][0], 'program_word_offset': i * stride, 'predicate': inst.predicate}
                  for i, inst in enumerate(instructions)]
    return {"header": header, "source": msl, "register_bytes": len(p.registers) * threads * 8, "program": program, "program_stride": stride, "program_sha256": hashlib.sha256(program).hexdigest(), "handler_count": len(variants), "input_names":["program","arena_in","weights","constants","params","resource_table","launch"],"output_names":["arena_out","diagnostics"],"param_bytes":p.param_bytes,"shared_bytes":p.shared_bytes,"opcode_coverage":dict(sorted(Counter(i.opcode for i in instructions).items())),"source_map":source_map,"collective_sites":sites,"declarations":p.declarations,"numerical_limits":["MMA uses ordered FP32 sums and per-instruction RNE half publication; NVIDIA tensor-core internal rounding is not proven identical.","SFU uses Metal approximations with the ops.metal domain/error contract; NVIDIA bit identity is not claimed.","Texture linear fractional weights use CUDA 8-bit fraction precision; nonzero mip levels are rejected at runtime.","Serial virtual CTAs and block scheduler preserve defined synchronized memory effects; races between original CUDA CTAs have no deterministic equivalence contract."],"source_sha256":hashlib.sha256(msl.encode()).hexdigest(),"ptx_sha256":hashlib.sha256(source.encode()).hexdigest(),"entry":entry,"instruction_count":len(instructions),"scheduler":"GPU instruction records; uniform collective scheduler; serial virtual CTAs","threadgroup_bytes":max(1,p.shared_bytes)+4*max(1,(p.shared_bytes+31)//32)+24*threads+12}


def compile_kernel(source, entry, dispatch_spec):
    """Compile the original serial-CTA GPU reference executor."""
    return _compile_kernel(source, entry, dispatch_spec)


def compile_resume(source, entry, dispatch_spec, phase_cuts):
    """Compile one original CTA, yielding before a scheduler-selected MMA cut.

    Cuts are (start_pc, end_pc) pairs with exclusive ends. All MMA PCs must be
    covered. The tensor executor replaces only selected-run lanes, then supplies
    their updated raw registers and PCs on the next invocation. Inputs are never
    mutated; every output checkpoint is published even on done or fault.
    resume_info[0] initializes exactly once per CTA; [1] selects its linear index.
    Callers serialize original CTAs and carry arena_out into the next invocation.
    """
    phase_cuts = tuple(tuple(cut) for cut in phase_cuts)
    compiled = _compile_kernel(source, entry, dispatch_spec, phase_cuts)
    parser = Parser(source, entry)
    block = tuple(int(x) for x in dispatch_spec['cuda_block'])
    threads = block[0] * block[1] * block[2]
    slots = {name: slot for slot, name in enumerate(parser.registers)}
    shapes = {'registers': [len(slots), threads],
              'shared': [max(1, parser.shared_bytes)],
              'defined': [max(1, (parser.shared_bytes + 31) // 32)],
              'control': [threads, 2]}
    compiled['input_names'] += ['registers_in', 'shared_in', 'defined_in', 'control_in', 'resume_info']
    compiled['output_names'] += ['registers_out', 'shared_out', 'defined_out', 'control_out', 'status']
    compiled.update(register_slots=slots, register_types=dict(parser.registers),
                    register_count=len(slots), checkpoint_shapes=shapes,
                    gpu_grid=[threads, 1, 1], gpu_threadgroup=[threads, 1, 1],
                    phase_cuts=sorted(tuple(cut) for cut in phase_cuts),
                    scheduler='One original CTA; original eligible collective scheduler; source-PC MMA yield',
                    checkpoint_abi={'schema': 'rr.scalar.resume.v1',
                                    'registers': {'dtype': 'uint64', 'shape': shapes['registers'], 'layout': 'slot-major raw bits'},
                                    'shared': {'dtype': 'uint8', 'shape': shapes['shared']},
                                    'defined': {'dtype': 'uint32', 'shape': shapes['defined'], 'layout': 'one bit per shared byte'},
                                    'control': {'dtype': 'uint32', 'shape': shapes['control'], 'columns': ['pc', 'selected_run']},
                                    'resume_info': {'dtype': 'uint32', 'shape': [8], 'fields': ['init', 'cta_linear', 'reserved', 'reserved', 'reserved', 'reserved', 'reserved', 'reserved']},
                                    'status': {'dtype': 'uint32', 'shape': [4], 'fields': ['event', 'selected_pc', 'cta_linear', 'reserved'], 'events': {'mma': 1, 'done': 2, 'fault': 3}},
                                    'diagnostics': {'dtype': 'uint32', 'shape': [4]},
                                    'end_pc': 0xffffffff})
    compiled['numerical_limits'][0] = 'No original MMA executes in scalar resume; selected source cuts require external exact ordered contraction.'
    return compiled


def _inventory(paths):
    from pathlib import Path
    union = {}
    for path in paths:
        source = Path(path).read_text()
        for entry in re.findall(r"\.entry\s+([\w$]+)", _clean(source)):
            parser = Parser(source, entry)
            print(path, entry, len(parser.instructions), parser.param_bytes, parser.shared_bytes)
            for inst in parser.instructions:
                union.setdefault(inst.opcode, inst.text)
    for op, example in sorted(union.items()):
        print(op, example.replace("\n", " "))


def _check(paths):
    """Emission-only check; fixture addresses are not captured dispatch bindings."""
    from pathlib import Path
    union=Counter(); total=0
    for path in paths:
        source=Path(path).read_text()
        for entry in re.findall(r"\.entry\s+([\w$]+)",_clean(source)):
            if entry=="PREPASS_ENTRYPOINT_NAME":
                continue
            parser=Parser(source,entry)
            symbols={name:{"virtual_base":0x400000000+index*0x10000,"arena_offset":index*0x10000,"bytes":size,"buffer":"constants"} for index,(name,size,_) in enumerate(parser.globals)}
            spec={"entry":entry,"cuda_grid":[1,1,1],"cuda_block":[32,1,1],"resources":[],"global_symbols":symbols}
            compiled=compile_kernel(source,entry,spec)
            assert len(compiled["source_map"])==len(parser.instructions)
            assert sum(compiled["opcode_coverage"].values())==len(parser.instructions)
            assert [m["instruction"] for m in compiled["source_map"]]==list(range(len(parser.instructions)))
            emitted_lines=compiled["source"].splitlines()
            program=struct.unpack(f'<{len(compiled["program"])//8}Q', compiled["program"])
            assert len(program)==len(parser.instructions)*compiled['program_stride']
            assert all(program[m['program_word_offset']]==m['handler'] and
                       1<=m['msl_line']<=m['msl_end_line']<=len(emitted_lines) for m in compiled['source_map'])
            assert "RR_GLOBAL" not in compiled["header"] and "RR_SHARED" not in compiled["header"]
            union.update(compiled["opcode_coverage"]);total+=len(parser.instructions)
            print(f"PASS {Path(path).name} {entry}: {len(parser.instructions)} instructions, {len(compiled['collective_sites'])} collective sites, {len(compiled['source'])} MSL bytes",flush=True)
    print(f"PASS source emission: {total} instructions, {len(union)} exact opcode/modifier tuples",flush=True)
    print("\n".join(sorted(union)))


def self_check():
    """Small parser/emission regression; GPU execution belongs to root QA."""
    source=r""".version 8.7
.target sm_89
.address_size 64
.visible .entry other(.param .b8 other_param[8]){trap;ret;}
.visible .entry wanted(.param .align 8 .b8 wanted_param[24]){
.reg .b32 %r<12>; .reg .pred %p<2>; .reg .f64 %fd<1>; .reg .b16 %h<1>;
.shared .align 16 .b8 scratch[32];
mov.u32 %r0, scratch;
{.reg .b32 low;mov.b32 low, 1;mov.b32 %r1,low;}
{.reg .b32 low;mov.b32 low, 2;mov.b32 %r2,low;}
mov.f64 %fd0, 0d3FF0000000000000;
cvt.rn.f16.f64 %h0,%fd0;
setp.eq.b32 %p0,%r1,%r2;
@!%p0 bra next;
@%p0 ret;
next:
mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16
{%r3,%r4},
{%r5,%r6,%r7,%r8},
{%r9,%r10},
{%r1,%r2};
bar.sync 0;ret;
}
"""+"\x00"
    spec={"entry":"wanted","cuda_grid":[1,1,1],"cuda_block":[32,1,1],"resources":[],"global_symbols":{}}
    parser=Parser(source,"wanted");compiled=compile_kernel(source,"wanted",spec)
    assert len(parser.instructions)==13
    assert len([name for name in parser.registers if name.endswith("_low")])==2
    assert parser.symbols["scratch"]==0x20000000
    assert compiled["opcode_coverage"]["mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16"]==1
    assert "trap" not in compiled["opcode_coverage"]
    program=struct.unpack(f'<{len(compiled["program"])//8}Q', compiled["program"])
    stride=compiled['program_stride']
    assert program[6*stride+12]==15360
    assert program[8*stride+4]==10 and program[8*stride+2]>>63==1
    assert program[9*stride+2]==program[8*stride+2]&((1<<63)-1)
    assert any(m["ptx_end_line"]-m["ptx_line"]==4 for m in compiled["source_map"])
    resumed=compile_resume(source,'wanted',spec,[(10,11)])
    assert resumed['register_slots']=={name: slot for slot,name in enumerate(parser.registers)}
    assert resumed['checkpoint_shapes']['registers']==[len(parser.registers),32]
    assert resumed['checkpoint_shapes']['defined']==[1]
    assert resumed['phase_cuts']==[(10,11)]
    assert 'rr_mma(' not in resumed['source']
    assert 'for(uint rr_cta_linear=' not in resumed['source']
    assert 'control_out[rr_thread*2u+1u]=rr_run[rr_thread]' in resumed['source']
    assert [(m['instruction'],m['ptx_offset'],m['ptx_end_offset'],m['predicate']) for m in resumed['source_map']]==[(m['instruction'],m['ptx_offset'],m['ptx_end_offset'],m['predicate']) for m in compiled['source_map']]
    resume_program=struct.unpack(f'<{len(resumed["program"])//8}Q',resumed['program'])
    assert [resume_program[pc*stride+7] for pc in range(len(parser.instructions))]==[int(pc==10) for pc in range(len(parser.instructions))]
    assert resume_program[8*stride+2]==program[8*stride+2]
    for cuts in ([],[(9,11)],[(10,11),(10,11)],[(10,14)]):
        try:
            compile_resume(source,'wanted',spec,cuts)
        except CompileError:
            pass
        else:
            raise AssertionError('Invalid or incomplete MMA cuts accepted')
    for bad_entry,bad_source in (("absent",source),("wanted",source.replace("mov.u32 %r0", "mov.bad %r0"))):
        try:
            compile_kernel(bad_source,bad_entry,{**spec,"entry":bad_entry})
        except CompileError:
            pass
        else:
            raise AssertionError("Unknown entry or semantic modifier was accepted")
    for fmt,ch in (("f16",4),("snorm8",4),("f32",1)):
        width=2 if fmt=="f16" else 1 if fmt=="snorm8" else 4
        resource={"handle":1,"virtual_base":4096,"arena_offset":0,"bytes":4*4*ch*width,"shape":[4,4,1,ch],"storage":{"dtype":fmt,"row_pitch":4*ch*width},"view":{"mode":0}}
        emitted=compile_kernel(source,"wanted",{**spec,"resources":[resource]})
        assert "rr_point_0" in emitted["header"]
        assert "RR_GLOBAL" not in emitted["header"]
    print("PASS exact entry, lexical scopes, multiline MMA, f64 fold, modifier diagnostics, original formatted view emission")


if __name__ == "__main__":
    import sys
    if sys.argv[1:2]==["--self-check"]:
        self_check()
    elif sys.argv[1:2]==["--check"]:
        _check(sys.argv[2:])
    else:
        _inventory(sys.argv[1:])
