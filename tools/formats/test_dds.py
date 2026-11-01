#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/dds.py. No game data
(no DDS ships in the reference install)."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import dds


def build_dds(w=4, h=4, fourcc=b"DXT1", mips=1, payload=None, hsize=124,
              caps=0x1000) -> bytes:
    if payload is None:
        block = dds.FOURCC_BLOCK_BYTES.get(fourcc, 8)
        total = 0
        ww, hh = w, h
        for _ in range(mips):
            total += max(1, (ww + 3) // 4) * max(1, (hh + 3) // 4) * block
            ww = max(1, ww // 2)
            hh = max(1, hh // 2)
        payload = bytes(total)
    hdr = bytearray(128)
    hdr[0:4] = dds.MAGIC
    struct.pack_into("<7I", hdr, 4, hsize, 0x100F, h, w, 0, 0, mips)
    struct.pack_into("<II4sI", hdr, 0x4C, 32, 0x4, fourcc, 0)
    struct.pack_into("<I", hdr, 0x74, caps)
    return bytes(hdr) + payload


class TestDds(unittest.TestCase):
    def test_dxt1_full_chain(self):
        d = dds.parse_dds(build_dds(w=8, h=8, mips=4))
        self.assertEqual((d.width, d.height), (8, 8))
        self.assertEqual(d.fourcc, b"DXT1")
        self.assertEqual(d.mipmaps, 4)

    def test_rgb_no_fourcc(self):
        hdr = bytearray(build_dds(w=2, h=2, fourcc=b"\x00\x00\x00\x00",
                                  mips=1, payload=bytes(2 * 2 * 3)))
        struct.pack_into("<II4sI", hdr, 0x4C, 32, 0x40 | 0x1, b"\x00" * 4, 24)
        struct.pack_into("<4I", hdr, 0x5C, 0x00FF0000, 0x0000FF00, 0x000000FF, 0)
        d = dds.parse_dds(bytes(hdr))
        self.assertEqual(d.rgb_bits, 24)

    def test_bad_magic(self):
        with self.assertRaises(dds.DdsError):
            dds.parse_dds(b"DDSX" + bytes(200))

    def test_bad_header_size(self):
        with self.assertRaises(dds.DdsError):
            dds.parse_dds(build_dds(hsize=120))

    def test_payload_size_mismatch(self):
        with self.assertRaises(dds.DdsError):
            dds.parse_dds(build_dds(w=8, h=8, mips=2, payload=bytes(10)))

    def test_unknown_fourcc(self):
        with self.assertRaises(dds.DdsError):
            dds.parse_dds(build_dds(fourcc=b"ZZZZ", payload=bytes(64)))

    def test_missing_texture_cap(self):
        with self.assertRaises(dds.DdsError):
            dds.parse_dds(build_dds(caps=0x2))


if __name__ == "__main__":
    unittest.main()
