#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/tga.py. No game data."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import tga


def header(img_type=2, w=4, h=4, bpp=24, id_len=0, cmap=0, desc=0) -> bytes:
    hdr = bytearray(18)
    hdr[0] = id_len
    hdr[1] = cmap
    hdr[2] = img_type
    struct.pack_into("<4H", hdr, 8, 0, 0, w, h)
    hdr[16] = bpp
    hdr[17] = desc
    return bytes(hdr)


class TestTga(unittest.TestCase):
    def test_rgb_raw_exact(self):
        data = header() + bytes(4 * 4 * 3)
        t = tga.parse_tga(data)
        self.assertEqual((t.width, t.height, t.bpp), (4, 4, 24))
        self.assertEqual(t.pixel_bytes, 48)

    def test_gray(self):
        t = tga.parse_tga(header(img_type=3, bpp=8) + bytes(16))
        self.assertEqual(t.pixel_bytes, 16)

    def test_with_id(self):
        t = tga.parse_tga(header(id_len=5) + b"id123" + bytes(48))
        self.assertEqual(t.id_length, 5)

    def test_rle_rgb(self):
        # one RLE packet covering all 16 pixels: header 0x80|15, then one pixel
        pix = bytes([1, 2, 3])
        stream = bytes([0x80 | 15]) + pix
        t = tga.parse_tga(header(img_type=10) + stream)
        self.assertEqual(t.pixel_bytes, 16 * 3)

    def test_rle_raw_packets(self):
        # two raw packets of 8 pixels each
        stream = bytes([7]) + bytes(8 * 3) + bytes([7]) + bytes(8 * 3)
        t = tga.parse_tga(header(img_type=10) + stream)
        self.assertEqual(t.pixel_bytes, 16 * 3)

    def test_trailing_bytes_allowed(self):
        # shipped lightmaps carry an opaque blob after the pixel stream
        t = tga.parse_tga(header() + bytes(48) + b"x")
        self.assertEqual(t.size, 67)

    def test_truevision_footer(self):
        foot = bytes(8) + b"TRUEVISION-XFILE.\x00"
        t = tga.parse_tga(header() + bytes(48) + foot)
        self.assertEqual(t.pixel_bytes, 48)

    def test_truncated_rejected(self):
        with self.assertRaises(tga.TgaError):
            tga.parse_tga(header() + bytes(10))

    def test_footer_but_short_pixels_rejected(self):
        foot = bytes(8) + b"TRUEVISION-XFILE.\x00"
        with self.assertRaises(tga.TgaError):
            tga.parse_tga(header() + bytes(40) + foot)

    def test_bad_type(self):
        with self.assertRaises(tga.TgaError):
            tga.parse_tga(header(img_type=7) + bytes(48))

    def test_bad_bpp(self):
        with self.assertRaises(tga.TgaError):
            tga.parse_tga(header(img_type=2, bpp=12) + bytes(48))

    def test_rle_overrun(self):
        with self.assertRaises(tga.TgaError):
            tga.parse_tga(header(img_type=10) + bytes([0x80 | 15]) + bytes(2))

    def test_short(self):
        with self.assertRaises(tga.TgaError):
            tga.parse_tga(b"\x00" * 10)


if __name__ == "__main__":
    unittest.main()
