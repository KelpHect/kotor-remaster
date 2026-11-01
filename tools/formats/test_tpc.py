#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/tpc.py. No game data."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import tpc
import txi


def build_tpc(w=4, h=4, enc=2, mips=1, data_size=None, txi_blob=b"mipmap 0 \r\n",
              alpha=1.0, compressed=True) -> bytes:
    hdr = bytearray(128)
    chain = tpc.chain_size(w, h, enc, mips, compressed)
    ds = chain if data_size is None else data_size
    struct.pack_into("<I", hdr, 0, ds if compressed else 0)
    struct.pack_into("<f", hdr, 4, alpha)
    struct.pack_into("<HH", hdr, 8, w, h)
    hdr[0x0C] = enc
    hdr[0x0D] = mips
    return bytes(hdr) + bytes(chain) + txi_blob


class TestTpc(unittest.TestCase):
    def test_dxt1_single_mip_with_txi(self):
        t = tpc.parse_tpc(build_tpc(), txi_parser=txi.parse_txi)
        self.assertEqual((t.width, t.height), (4, 4))
        self.assertEqual(t.encoding, 2)
        self.assertEqual(t.pixel_bytes, 8)
        self.assertEqual(t.txi, b"mipmap 0 \r\n")
        self.assertEqual(txi.parse_txi(t.txi)["mipmap"], "0")

    def test_dxt1_multi_mip(self):
        # 256x256 DXT1, 9 mips: chain = 43704, data_size = mip0 = 32768
        data = build_tpc(w=256, h=256, enc=2, mips=9, txi_blob=b"")
        hdr = bytearray(data[:128])
        struct.pack_into("<I", hdr, 0, 32768)
        t = tpc.parse_tpc(bytes(hdr) + data[128:], txi_parser=txi.parse_txi)
        self.assertEqual(t.data_size, 32768)
        self.assertEqual(t.pixel_bytes, 43704)

    def test_raw_when_data_size_zero(self):
        # enc 0xC BGRA raw: 4x4x4 = 64 bytes, data_size == 0
        t = tpc.parse_tpc(build_tpc(w=4, h=4, enc=0xC, mips=1, data_size=0,
                                    compressed=False, txi_blob=b""))
        self.assertEqual(t.data_size, 0)
        self.assertEqual(t.pixel_bytes, 64)

    def test_bad_encoding(self):
        data = bytearray(build_tpc())
        data[0x0C] = 0x55
        with self.assertRaises(tpc.TpcError):
            tpc.parse_tpc(bytes(data))

    def test_data_size_not_chain_still_parses(self):
        # a few shipped GUI textures carry a data_size that is not the chain
        # size; the validator then falls back to the chain interpretation
        data = build_tpc(w=8, h=8, enc=2, mips=1)
        struct.pack_into("<I", bytearray(data), 0, 0)  # placeholder no-op
        t = tpc.parse_tpc(build_tpc(w=8, h=8, enc=2, mips=1))
        self.assertEqual(t.pixel_bytes, 32)

    def test_nothing_fits(self):
        data = build_tpc(w=64, h=64, enc=2, mips=1, txi_blob=b"")
        # truncate so no chain interpretation fits
        with self.assertRaises(tpc.TpcError):
            tpc.parse_tpc(data[:-100])

    def test_truncated_chain(self):
        data = build_tpc(w=64, h=64, enc=2, mips=1, txi_blob=b"")[:-4]
        with self.assertRaises(tpc.TpcError):
            tpc.parse_tpc(data)

    def test_bad_dims(self):
        data = bytearray(build_tpc())
        struct.pack_into("<H", data, 8, 0)
        with self.assertRaises(tpc.TpcError):
            tpc.parse_tpc(bytes(data))


if __name__ == "__main__":
    unittest.main()
