#!/usr/bin/env python3
"""Clean-room NCS V1.0 codec (KotOR 1 NWScript compiled script).

Promoted from the lane-04 verification codec
(``artifacts/lanes/04-ncs/cont/tools/ncs_encode.py``), which was written
independently of the lane's validator so the encoder was not tested against
the decoder it was derived from. Layout facts used here are the documented
big-endian NCS V1.0 form only:

    offset 0   char[8]  b'NCS V1.0'
    offset 8   u8       0x42  (script-size pseudo-op)
    offset 9   u32 BE   total file size including this header
    offset 13  instruction stream: opcode byte, qualifier byte where the
               opcode takes one, big-endian operands

Verified on the shipped corpus (build SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6):
8,666/8,666 unique NCS decode and re-encode byte-identically in both
readback and semantic modes; see docs/recon/20_SCRIPT_CORPUS.md.

The opcode table below is bytecode-format knowledge, not decompiled program
text. The 772-entry action id->name map is *not* embedded here; load it with
``actions.load_action_table`` from the artifact produced by
``tools/analysis/extract_actions.py``.

CLI:
    python -m tools.scripts.ncs --corpus <dir-of-.ncs> --out report.json
    python -m tools.scripts.ncs --one <file.ncs>
"""
from __future__ import annotations

import argparse
import io
import json
import os
import struct
import sys
from dataclasses import dataclass, field

MAGIC = b'NCS V1.0'
HEADER_SIZE = 13
SCRIPT_SIZE_OP = 0x42

# opcode -> (mnemonic, operand shape)
#   none        : opcode only
#   qual        : qualifier byte
#   copy        : qual + i32 offset + u16 size
#   const       : type byte (as qualifier) + payload (i32 / f32 / u16 len + bytes / i32)
#   action      : qual + u16 id + u8 argc
#   eq          : qual (+ u16 struct size when qual == 0x24)
#   jump        : qual + i32 relative offset
#   destruct    : qual + u16 size + u16 type + u16 extra
#   storestate : qual + i32 + i32
#   size        : u32
SHAPES = {
    0x01: ('CPDOWNSP', 'copy'), 0x02: ('RSADD', 'qual'), 0x03: ('CPTOPSP', 'copy'),
    0x04: ('CONST', 'const'), 0x05: ('ACTION', 'action'), 0x06: ('LOGAND', 'qual'),
    0x07: ('LOGOR', 'qual'), 0x08: ('INCOR', 'qual'), 0x09: ('EXCOR', 'qual'),
    0x0A: ('BOOLAND', 'qual'), 0x0B: ('EQUAL', 'eq'), 0x0C: ('NEQUAL', 'eq'),
    0x0D: ('GEQ', 'qual'), 0x0E: ('GT', 'qual'), 0x0F: ('LT', 'qual'), 0x10: ('LEQ', 'qual'),
    0x11: ('SHLEFT', 'qual'), 0x12: ('SHRIGHT', 'qual'), 0x13: ('USHRIGHT', 'qual'),
    0x14: ('ADD', 'qual'), 0x15: ('SUB', 'qual'), 0x16: ('MUL', 'qual'),
    0x17: ('DIV', 'qual'), 0x18: ('MOD', 'qual'), 0x19: ('NEG', 'qual'),
    0x1A: ('COMP', 'qual'), 0x1B: ('MOVSP', 'jump'), 0x1C: ('STORESTATEALL', 'qual'),
    0x1D: ('JMP', 'jump'), 0x1E: ('JSR', 'jump'), 0x1F: ('JZ', 'jump'),
    0x20: ('RETN', 'qual'), 0x21: ('DESTRUCT', 'destruct'), 0x22: ('NOT', 'qual'),
    0x23: ('DECSP', 'jump'), 0x24: ('INCSP', 'jump'), 0x25: ('JNZ', 'jump'),
    0x26: ('CPDOWNBP', 'copy'), 0x27: ('CPTOPBP', 'copy'), 0x28: ('DECBP', 'jump'),
    0x29: ('INCBP', 'jump'), 0x2A: ('SAVEBP', 'qual'), 0x2B: ('RESTOREBP', 'qual'),
    0x2C: ('STORESTATE', 'storestate'), 0x2D: ('NOP', 'qual'),
    0x42: ('SCRIPTSIZE', 'size'),
}

CONST_INT, CONST_FLOAT, CONST_STRING, CONST_OBJECT = 0x03, 0x04, 0x05, 0x06

OP_ACTION = 0x05


class DecodeError(Exception):
    """Raised when a payload is not a well-formed NCS V1.0 stream."""


@dataclass
class Instruction:
    """One decoded instruction.

    ``ops`` holds decoded operands as (kind, value) tuples with kind in
    'i' (i32), 'u' (u16/u32), 'f' (f32), 'b' (bytes), 'c' (raw operand bytes).
    ``raw`` holds the operand bytes exactly as seen (excluding opcode and
    qualifier) for lossless readback re-emission.
    """
    offset: int
    opcode: int
    name: str
    shape: str
    qualifier: int | None = None
    ops: list = field(default_factory=list)
    raw: bytes = b''
    size: int = 0

    @property
    def action(self) -> tuple[int, int] | None:
        """(action_id, argc) for ACTION instructions, else None."""
        if self.opcode == OP_ACTION and self.ops:
            blob = self.ops[0][1]  # 3 operand bytes: u16 id + u8 argc
            return struct.unpack_from('>H', blob, 0)[0], blob[2]
        return None


def decode(data: bytes) -> list[Instruction]:
    """Decode a full NCS payload (header + instruction stream) to instructions."""
    if len(data) < HEADER_SIZE or data[:8] != MAGIC:
        raise DecodeError('bad header')
    if data[8] != SCRIPT_SIZE_OP:
        raise DecodeError('missing 0x42 size op')
    if struct.unpack_from('>I', data, 9)[0] != len(data):
        raise DecodeError('declared size mismatch')
    out: list[Instruction] = []
    p = HEADER_SIZE
    n = len(data)
    while p < n:
        start = p
        op = data[p]
        ent = SHAPES.get(op)
        if ent is None:
            raise DecodeError('unknown opcode 0x%02X at 0x%X' % (op, p))
        name, shape = ent
        p += 1
        ins = Instruction(offset=start, opcode=op, name=name, shape=shape)
        body_start = p
        if shape in ('qual', 'copy', 'const', 'action', 'eq', 'jump',
                     'destruct', 'storestate'):
            if p >= n:
                raise DecodeError('truncated qualifier at 0x%X' % start)
            ins.qualifier = data[p]
            p += 1
        if shape == 'copy':
            ins.ops = [('i', struct.unpack_from('>i', data, p)[0]),
                       ('u', struct.unpack_from('>H', data, p + 4)[0])]
            p += 6
        elif shape == 'const':
            ct = ins.qualifier
            if ct == CONST_INT:
                ins.ops = [('i', struct.unpack_from('>i', data, p)[0])]
                p += 4
            elif ct == CONST_FLOAT:
                ins.ops = [('f', struct.unpack_from('>f', data, p)[0])]
                p += 4
            elif ct == CONST_STRING:
                ln = struct.unpack_from('>H', data, p)[0]
                if p + 2 + ln > n:
                    raise DecodeError('string overrun at 0x%X' % start)
                ins.ops = [('b', data[p + 2:p + 2 + ln])]
                p += 2 + ln
            elif ct == CONST_OBJECT:
                ins.ops = [('i', struct.unpack_from('>i', data, p)[0])]
                p += 4
            else:
                raise DecodeError('bad CONST type 0x%02X at 0x%X' % (ct, start))
        elif shape == 'action':
            ins.ops = [('c', data[p:p + 3])]
            p += 3
        elif shape == 'eq':
            if ins.qualifier == 0x24:
                ins.ops = [('u', struct.unpack_from('>H', data, p)[0])]
                p += 2
        elif shape == 'jump':
            ins.ops = [('i', struct.unpack_from('>i', data, p)[0])]
            p += 4
        elif shape == 'destruct':
            ins.ops = [('u', struct.unpack_from('>H', data, p)[0]),
                       ('u', struct.unpack_from('>H', data, p + 2)[0]),
                       ('u', struct.unpack_from('>H', data, p + 4)[0])]
            p += 6
        elif shape == 'storestate':
            ins.ops = [('i', struct.unpack_from('>i', data, p)[0]),
                       ('i', struct.unpack_from('>i', data, p + 4)[0])]
            p += 8
        elif shape == 'size':
            ins.ops = [('u', struct.unpack_from('>I', data, p)[0])]
            p += 4
        if p > n:
            raise DecodeError('truncated operand at 0x%X' % start)
        ins.raw = data[body_start:p]
        ins.size = p - start
        out.append(ins)
    return out


def _body(ins: Instruction, mode: str) -> bytes:
    b = io.BytesIO()
    b.write(bytes([ins.opcode]))
    if mode == 'readback':
        b.write(ins.raw)
        return b.getvalue()
    # semantic re-emission from decoded values
    if ins.qualifier is not None:
        b.write(bytes([ins.qualifier]))
    if ins.shape == 'copy':
        b.write(struct.pack('>i', ins.ops[0][1]))
        b.write(struct.pack('>H', ins.ops[1][1]))
    elif ins.shape == 'const':
        if ins.qualifier in (CONST_INT, CONST_OBJECT):
            b.write(struct.pack('>i', ins.ops[0][1]))
        elif ins.qualifier == CONST_FLOAT:
            b.write(struct.pack('>f', ins.ops[0][1]))
        elif ins.qualifier == CONST_STRING:
            s = ins.ops[0][1]
            b.write(struct.pack('>H', len(s)))
            b.write(s)
    elif ins.shape == 'action':
        b.write(ins.ops[0][1])
    elif ins.shape == 'eq':
        if ins.qualifier == 0x24:
            b.write(struct.pack('>H', ins.ops[0][1]))
    elif ins.shape == 'jump':
        b.write(struct.pack('>i', ins.ops[0][1]))
    elif ins.shape == 'destruct':
        for _, v in ins.ops:
            b.write(struct.pack('>H', v))
    elif ins.shape == 'storestate':
        for _, v in ins.ops:
            b.write(struct.pack('>i', v))
    elif ins.shape == 'size':
        b.write(struct.pack('>I', ins.ops[0][1]))
    return b.getvalue()


def encode(instructions: list[Instruction], mode: str = 'readback',
           total_override: int | None = None) -> bytes:
    """Re-emit a decoded instruction stream behind the 13-byte header.

    ``mode='readback'`` re-emits operand bytes exactly as decoded (tests
    framing/order/size); ``mode='semantic'`` re-packs operands from decoded
    values (tests value fidelity).
    """
    if mode not in ('readback', 'semantic'):
        raise ValueError('mode must be readback or semantic')
    body = b''.join(_body(i, mode) for i in instructions)
    total = HEADER_SIZE + len(body) if total_override is None else total_override
    return MAGIC + bytes([SCRIPT_SIZE_OP]) + struct.pack('>I', total) + body


def roundtrip(data: bytes) -> dict:
    """Decode ``data`` and re-encode in both modes; report byte equality."""
    insns = decode(data)
    rb = encode(insns, 'readback')
    sem = encode(insns, 'semantic')
    return {
        'ok': True,
        'size': len(data),
        'instructions': len(insns),
        'readback_identical': rb == data,
        'semantic_identical': sem == data,
    }


def iter_actions(data: bytes):
    """Yield (offset, action_id, argc) for every ACTION instruction."""
    for ins in decode(data):
        a = ins.action
        if a is not None:
            yield ins.offset, a[0], a[1]


def first_diff(a: bytes, b: bytes) -> tuple[int, str | None]:
    """Return (offset, kind) of first divergence, or (-1, None) if equal."""
    if a == b:
        return -1, None
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i, 'value'
    return n, 'length'


def check_bytes(data: bytes) -> dict:
    """Full per-payload check: decode + both re-encode modes + action scan."""
    rec = {'size': len(data)}
    try:
        insns = decode(data)
    except DecodeError as e:
        rec.update(ok=False, error=str(e))
        return rec
    rb = encode(insns, 'readback')
    sem = encode(insns, 'semantic')
    actions = {}
    for ins in insns:
        a = ins.action
        if a is not None:
            actions[a[0]] = actions.get(a[0], 0) + 1
    rec.update(ok=True, instructions=len(insns),
               readback_identical=rb == data, semantic_identical=sem == data,
               action_ops=sum(actions.values()), action_ids=sorted(actions),
               actions=actions)
    return rec


def check_file(path: str) -> dict:
    with open(path, 'rb') as f:
        data = f.read()
    rec = check_bytes(data)
    rec['file'] = os.path.basename(path)
    return rec


def run_corpus_dir(corpus: str, out: str | None, jobs: int) -> dict:
    """Round-trip every ``*.ncs`` in ``corpus`` (multiprocessed)."""
    from concurrent.futures import ProcessPoolExecutor
    files = sorted(os.path.join(corpus, f) for f in os.listdir(corpus)
                   if f.lower().endswith('.ncs'))
    rows = []
    if jobs > 1 and len(files) > 64:
        with ProcessPoolExecutor(max_workers=jobs) as ex:
            rows = list(ex.map(check_file, files, chunksize=64))
    else:
        rows = [check_file(f) for f in files]
    ok = sum(1 for r in rows if r.get('ok') and r.get('readback_identical'))
    sem = sum(1 for r in rows if r.get('ok') and r.get('semantic_identical'))
    bad = [r for r in rows if not (r.get('ok') and r.get('readback_identical'))]
    summary = {
        'tool': 'tools/scripts/ncs.py',
        'mode': 'decode -> re-emit big-endian (13-byte header + 0x42 size) -> byte compare',
        'files': len(rows),
        'readback_exact': ok,
        'semantic_exact': sem,
        'readback_mismatch': len(bad),
        'total_instructions': sum(r.get('instructions', 0) for r in rows),
        'total_action_ops': sum(r.get('action_ops', 0) for r in rows),
        'mismatches': bad,
    }
    if out:
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        with open(out, 'w', encoding='utf-8') as f:
            json.dump(summary, f, indent=2, sort_keys=True)
    return summary


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--corpus', help='directory of .ncs files to round-trip')
    ap.add_argument('--out', help='JSON report path for --corpus')
    ap.add_argument('--jobs', type=int, default=min(16, (os.cpu_count() or 4)))
    ap.add_argument('--one', help='check a single .ncs file')
    args = ap.parse_args()
    if args.one:
        print(json.dumps(check_file(args.one), indent=2, sort_keys=True))
        return 0
    if not args.corpus:
        ap.error('--corpus or --one required')
    summary = run_corpus_dir(args.corpus, args.out, args.jobs)
    print(json.dumps({k: v for k, v in summary.items() if k != 'mismatches'},
                     indent=2, sort_keys=True))
    print('mismatches:', len(summary['mismatches']))
    return 0 if summary['readback_mismatch'] == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
