#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/ssf.py. No game data."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ssf


def build(n=41, lead=ssf.LEAD, magic=ssf.MAGIC) -> bytes:
    out = bytearray(magic)
    out += struct.pack("<I", lead)
    for i in range(n):
        out += struct.pack("<I", 1000 + i if i % 3 else ssf.EMPTY)
    return bytes(out)


class TestSsf(unittest.TestCase):
    def test_valid(self):
        s = ssf.parse_ssf(build())
        self.assertEqual(len(s.strrefs), 41)
        self.assertEqual(s.strrefs[1], 1001)
        self.assertEqual(s.strrefs[0], ssf.EMPTY)

    def test_size_208(self):
        s = ssf.parse_ssf(build(n=50))
        self.assertEqual(len(s.strrefs), 50)

    def test_bad_magic(self):
        with self.assertRaises(ssf.SsfError):
            ssf.parse_ssf(b"SSF V1.0" + bytes(200))

    def test_bad_lead(self):
        with self.assertRaises(ssf.SsfError):
            ssf.parse_ssf(build(lead=7))

    def test_unaligned(self):
        with self.assertRaises(ssf.SsfError):
            ssf.parse_ssf(ssf.MAGIC + struct.pack("<I", 12) + b"\x01\x02\x03")

    def test_short(self):
        with self.assertRaises(ssf.SsfError):
            ssf.parse_ssf(b"SSF V1.1")


if __name__ == "__main__":
    unittest.main()
