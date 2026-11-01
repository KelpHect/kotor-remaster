#!/usr/bin/env python3
"""Synthetic-fixture tests for the promoted NCS codec (tools/scripts/ncs.py).

No game data is used; every fixture is built in-memory with the encoder
itself plus raw byte edits. Run from the repo root:

    python -m unittest discover -s tools/scripts/tests -v
or:
    python tools/scripts/tests/test_ncs.py
"""
from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

from scripts import ncs  # noqa: E402


def ins(op, shape, qual=None, ops=None, raw=None):
    i = ncs.Instruction(offset=0, opcode=op, name=ncs.SHAPES[op][0], shape=shape,
                        qualifier=qual, ops=ops or [])
    if raw is not None:
        i.raw = raw
    else:
        # derive raw the way decode() would, via encode of a single instruction
        i.raw = ncs.encode([i], 'semantic')[ncs.HEADER_SIZE + 1:]
    i.size = 1 + len(i.raw)
    return i


def make_script(instructions) -> bytes:
    """Encode an instruction list into a full NCS payload."""
    return ncs.encode(instructions, 'readback')


def sample_script() -> bytes:
    """A small synthetic script exercising copy/const/action/jump shapes."""
    return make_script([
        ins(0x04, 'const', qual=ncs.CONST_INT, ops=[('i', 41)]),
        ins(0x04, 'const', qual=ncs.CONST_INT, ops=[('i', 1)]),
        ins(0x14, 'qual', qual=0x20, ops=[], raw=b'\x20'),
        ins(0x04, 'const', qual=ncs.CONST_STRING, ops=[('b', b'hello')]),
        ins(0x04, 'const', qual=ncs.CONST_FLOAT, ops=[('f', 2.5)]),
        ins(0x04, 'const', qual=ncs.CONST_OBJECT, ops=[('i', -2002)]),
        ins(0x05, 'action', qual=0x20, ops=[('c', struct.pack('>HB', 6, 2))]),
        ins(0x05, 'action', qual=0x20, ops=[('c', struct.pack('>HB', 574, 1))]),
        ins(0x0B, 'eq', qual=0x24, ops=[('u', 12)]),
        ins(0x1D, 'jump', qual=0x03, ops=[('i', 4)]),
        ins(0x01, 'copy', qual=0x10, ops=[('i', -8), ('u', 4)]),
        ins(0x21, 'destruct', qual=0x10, ops=[('u', 4), ('u', 3), ('u', 0)]),
        ins(0x2C, 'storestate', qual=0x02, ops=[('i', 16), ('i', 20)]),
        ins(0x20, 'qual', qual=0x03, ops=[], raw=b'\x03'),
        ins(0x2D, 'qual', qual=0x00, ops=[], raw=b'\x00'),
    ])


class TestCodec(unittest.TestCase):
    def test_roundtrip_readback_and_semantic(self):
        data = sample_script()
        r = ncs.roundtrip(data)
        self.assertTrue(r['ok'])
        self.assertTrue(r['readback_identical'])
        self.assertTrue(r['semantic_identical'])
        self.assertEqual(r['size'], len(data))
        self.assertEqual(r['instructions'], 15)

    def test_decode_encode_idempotent(self):
        data = sample_script()
        once = ncs.encode(ncs.decode(data), 'semantic')
        twice = ncs.encode(ncs.decode(once), 'semantic')
        self.assertEqual(once, data)
        self.assertEqual(twice, data)

    def test_header_fields(self):
        data = sample_script()
        self.assertEqual(data[:8], b'NCS V1.0')
        self.assertEqual(data[8], 0x42)
        self.assertEqual(struct.unpack_from('>I', data, 9)[0], len(data))

    def test_action_extraction(self):
        data = sample_script()
        acts = list(ncs.iter_actions(data))
        self.assertEqual([(a[1], a[2]) for a in acts], [(6, 2), (574, 1)])
        # 13-byte header + CONST int(6)+CONST int(6)+ADD(2)+CONST str(9)+CONST float(6)+CONST obj(6)
        self.assertEqual(acts[0][0], 13 + 6 + 6 + 2 + 9 + 6 + 6)

    def test_check_bytes_action_counts(self):
        data = sample_script()
        rec = ncs.check_bytes(data)
        self.assertTrue(rec['ok'])
        self.assertEqual(rec['action_ops'], 2)
        self.assertEqual(rec['actions'], {6: 1, 574: 1})
        self.assertEqual(rec['action_ids'], [6, 574])

    def test_bad_magic(self):
        with self.assertRaises(ncs.DecodeError):
            ncs.decode(b'NOPE V1.0' + b'\x42' + struct.pack('>I', 13))

    def test_missing_size_op(self):
        data = bytearray(sample_script())
        data[8] = 0x00
        with self.assertRaises(ncs.DecodeError):
            ncs.decode(bytes(data))

    def test_declared_size_mismatch(self):
        data = bytearray(sample_script())
        data[9] ^= 0xFF
        with self.assertRaises(ncs.DecodeError):
            ncs.decode(bytes(data))

    def test_unknown_opcode(self):
        data = sample_script() + b'\x7F\x00'
        data = data[:9] + struct.pack('>I', len(data)) + data[13:]
        with self.assertRaises(ncs.DecodeError) as cm:
            ncs.decode(data)
        self.assertIn('unknown opcode 0x7F', str(cm.exception))

    def test_truncated_operand(self):
        data = sample_script()[:-1]  # chop the NOP qualifier byte
        data = data[:9] + struct.pack('>I', len(data)) + data[13:]
        with self.assertRaises(ncs.DecodeError):
            ncs.decode(data)

    def test_truncated_string(self):
        bad = make_script([ins(0x04, 'const', qual=ncs.CONST_STRING, ops=[('b', b'abc')])])
        bad = bad[:-1]  # chop one string byte, fix declared size
        bad = bad[:9] + struct.pack('>I', len(bad)) + bad[13:]
        with self.assertRaises(ncs.DecodeError):
            ncs.decode(bad)

    def test_first_diff(self):
        self.assertEqual(ncs.first_diff(b'abc', b'abc'), (-1, None))
        self.assertEqual(ncs.first_diff(b'abc', b'abd'), (2, 'value'))
        self.assertEqual(ncs.first_diff(b'abc', b'ab'), (2, 'length'))

    def test_encode_mode_validation(self):
        with self.assertRaises(ValueError):
            ncs.encode([], 'bogus')

    def test_total_override(self):
        data = make_script([ins(0x2D, 'qual', qual=0, raw=b'\x00')])
        out = ncs.encode(ncs.decode(data), 'readback', total_override=999)
        self.assertEqual(struct.unpack_from('>I', out, 9)[0], 999)

    def test_empty_stream(self):
        data = make_script([])
        self.assertEqual(len(data), ncs.HEADER_SIZE)
        self.assertEqual(ncs.decode(data), [])
        self.assertTrue(ncs.roundtrip(data)['readback_identical'])

    def test_all_opcodes_decode(self):
        insns = []
        for op, (name, shape) in sorted(ncs.SHAPES.items()):
            if shape == 'none':
                insns.append(ins(op, shape))
            elif shape == 'qual':
                insns.append(ins(op, shape, qual=0x03, raw=b'\x03'))
            elif shape == 'copy':
                insns.append(ins(op, shape, qual=0x10, ops=[('i', 4), ('u', 4)]))
            elif shape == 'const':
                insns.append(ins(op, shape, qual=ncs.CONST_INT, ops=[('i', 7)]))
            elif shape == 'action':
                insns.append(ins(op, shape, qual=0x20, ops=[('c', b'\x00\x06\x02')]))
            elif shape == 'eq':
                insns.append(ins(op, shape, qual=0x03, raw=b'\x03'))
            elif shape == 'jump':
                insns.append(ins(op, shape, qual=0x03, ops=[('i', 0)]))
            elif shape == 'destruct':
                insns.append(ins(op, shape, qual=0x10, ops=[('u', 4), ('u', 3), ('u', 1)]))
            elif shape == 'storestate':
                insns.append(ins(op, shape, qual=0x02, ops=[('i', 0), ('i', 4)]))
            elif shape == 'size':
                insns.append(ins(op, shape, qual=None, ops=[('u', 0)]))
        data = make_script(insns)
        rec = ncs.check_bytes(data)
        self.assertTrue(rec['ok'], rec.get('error'))
        self.assertEqual(rec['instructions'], len(ncs.SHAPES))
        self.assertTrue(rec['readback_identical'])
        self.assertTrue(rec['semantic_identical'])


if __name__ == '__main__':
    unittest.main()
