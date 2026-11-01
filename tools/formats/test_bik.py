#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/bik.py. No game data."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bik


def build(frames=3, width=640, height=272, version=b"i", size_delta=8,
          offsets=None, largest=100) -> bytes:
    table = list(offsets) if offsets is not None else None
    end_of_table = bik.TABLE_OFFSET + (frames + 1) * 4
    if table is None:
        table = [end_of_table + 100 * (i + 1) for i in range(frames)]
        table.append(end_of_table + 100 * (frames + 1))  # EOF sentinel
    total = max(table)
    hdr = bytearray(bik.TABLE_OFFSET)
    hdr[0:3] = b"BIK"
    hdr[3:4] = version
    struct.pack_into("<6I", hdr, 4, total - size_delta, frames, largest, frames,
                     width, height)
    # remaining header fields at 0x1C..0x37 left zero
    table_bytes = b"".join(struct.pack("<I", o) for o in table)
    return bytes(hdr) + table_bytes + bytes(total - end_of_table)


class TestBik(unittest.TestCase):
    def test_valid(self):
        data = build()
        b = bik.parse_bik(data)
        self.assertEqual(b.version, "i")
        self.assertEqual(b.frames, 3)
        self.assertEqual(b.width, 640)
        self.assertEqual(b.frame_offsets[-1], len(data))

    def test_bad_magic(self):
        with self.assertRaises(bik.BikError):
            bik.parse_bik(b"BIK".replace(b"BIK", b"XYK") + b"i" + bytes(100))

    def test_bad_version(self):
        with self.assertRaises(bik.BikError):
            bik.parse_bik(build(version=b"z"))

    def test_size_field_mismatch(self):
        with self.assertRaises(bik.BikError):
            bik.parse_bik(build(size_delta=4))

    def test_frame_count_mismatch(self):
        data = bytearray(build())
        struct.pack_into("<I", data, 0x10, 99)
        with self.assertRaises(bik.BikError):
            bik.parse_bik(bytes(data))

    def test_non_ascending_table(self):
        data = build(offsets=[bik.TABLE_OFFSET + 300, bik.TABLE_OFFSET + 200, bik.TABLE_OFFSET + 400])
        with self.assertRaises(bik.BikError):
            bik.parse_bik(data)

    def test_last_offset_must_be_filesize(self):
        data = bytearray(build())
        struct.pack_into("<I", data, bik.TABLE_OFFSET + 4 * 3, len(data) - 1)
        with self.assertRaises(bik.BikError):
            bik.parse_bik(bytes(data))

    def test_bad_dims(self):
        with self.assertRaises(bik.BikError):
            bik.parse_bik(build(width=0))

    def test_header_only(self):
        b = bik.parse_bik(build(), full=False)
        self.assertEqual(b.frame_offsets, ())


if __name__ == "__main__":
    unittest.main()
