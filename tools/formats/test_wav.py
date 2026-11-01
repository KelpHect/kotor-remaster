#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/wav.py. No game data."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import wav


def build_wav(pcm_bytes=b"\x00\x01" * 100, prefix=b"", channels=1, rate=22050,
              bits=16, fmt_extra=b"", data_size=None, riff_size_delta=0) -> bytes:
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * bits // 8,
                      channels * bits // 8, bits) + fmt_extra
    ds = len(pcm_bytes) if data_size is None else data_size
    body = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    body += b"data" + struct.pack("<I", ds) + pcm_bytes
    riff = b"WAVE" + body
    blob = b"RIFF" + struct.pack("<I", len(riff) + riff_size_delta) + riff
    return prefix + blob


class TestWav(unittest.TestCase):
    def test_pcm(self):
        w = wav.parse_wav(build_wav())
        self.assertEqual(w.prefix, 0)
        self.assertEqual(w.fmt["sample_rate"], 22050)
        self.assertEqual(w.fmt["bits"], 16)
        self.assertEqual(w.data_size, 200)
        self.assertEqual([c.id for c in w.chunks], [b"fmt ", b"data"])

    def test_prefix(self):
        w = wav.parse_wav(build_wav(prefix=b"\xff\xfb\x90\x00" + bytes(466)))
        self.assertEqual(w.prefix, 470)

    def test_odd_chunk_padding(self):
        # LIST chunk with odd size must be padded
        odd = b"LIST" + struct.pack("<I", 3) + b"abc\x00"
        fmt = struct.pack("<HHIIHH", 1, 1, 8000, 8000, 1, 8)
        body = odd + b"fmt " + struct.pack("<I", len(fmt)) + fmt \
            + b"data" + struct.pack("<I", 4) + b"\x00" * 4
        blob = b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body
        w = wav.parse_wav(blob)
        self.assertEqual(len(w.chunks), 3)

    def test_riff_size_mismatch(self):
        with self.assertRaises(wav.WavError):
            wav.parse_wav(build_wav(riff_size_delta=4))

    def test_no_data_chunk(self):
        fmt = struct.pack("<HHIIHH", 1, 1, 8000, 8000, 1, 8)
        body = b"fmt " + struct.pack("<I", len(fmt)) + fmt
        blob = b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body
        with self.assertRaises(wav.WavError):
            wav.parse_wav(blob)

    def test_no_riff(self):
        with self.assertRaises(wav.WavError):
            wav.parse_wav(b"\x00" * 600)

    def test_bad_channels(self):
        with self.assertRaises(wav.WavError):
            wav.parse_wav(build_wav(channels=0))

    def test_chunk_overrun(self):
        fmt = struct.pack("<HHIIHH", 1, 1, 8000, 8000, 1, 8)
        body = b"fmt " + struct.pack("<I", len(fmt)) + fmt \
            + b"data" + struct.pack("<I", 10_000) + b"\x00" * 10
        blob = b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body
        with self.assertRaises(wav.WavError):
            wav.parse_wav(blob)

    def test_file_size_param(self):
        # header prefix only + real size larger
        data = build_wav(pcm_bytes=b"\x00" * 1000)
        w = wav.parse_wav(data[:64], file_size=len(data))
        self.assertEqual(w.size, len(data))
        self.assertEqual(w.data_size, 1000)


if __name__ == "__main__":
    unittest.main()
