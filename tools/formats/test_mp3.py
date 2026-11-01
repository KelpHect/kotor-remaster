#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/mp3.py. No game data."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import mp3


def frame(size_payload, version=3, layer=1, br_idx=9, sr_idx=0, padding=0) -> bytes:
    """Build one MPEG frame header + payload.  br_idx 9 = 128 kbps (MPEG1 L3),
    sr_idx 0 = 44100 Hz -> frame size = 144*128000/44100 = 417 (+padding)."""
    b1 = 0xE0 | (version << 3) | (layer << 1)
    b2 = (br_idx << 4) | (sr_idx << 2) | (padding << 1)
    return bytes([0xFF, b1, b2, 0x00]) + bytes(size_payload)


def frame_size(br_idx=9, sr_idx=0, padding=0, version=3, layer=1):
    return mp3._frame_size(version, layer, br_idx, sr_idx, padding)[0]


class TestMp3(unittest.TestCase):
    def test_walk(self):
        n = 3
        sz = frame_size()
        blob = b"".join(frame(sz - 4) for _ in range(n))
        s = mp3.parse_mp3_stream(blob)
        self.assertEqual(len(s.frames), n)
        self.assertEqual(s.end, len(blob))
        self.assertEqual(s.frames[0].bitrate, 128000)
        self.assertEqual(s.frames[0].sample_rate, 44100)

    def test_padding_changes_size(self):
        sz0 = frame_size(padding=0)
        sz1 = frame_size(padding=1)
        self.assertEqual(sz1, sz0 + 1)
        blob = frame(sz0 - 4) + frame(sz1 - 4, padding=1)
        s = mp3.parse_mp3_stream(blob)
        self.assertEqual(len(s.frames), 2)

    def test_lost_sync(self):
        sz = frame_size()
        blob = frame(sz - 4) + b"\x00\x00\x00\x00" + frame(sz - 4)
        with self.assertRaises(mp3.Mp3Error):
            mp3.parse_mp3_stream(blob)

    def test_truncated_frame(self):
        sz = frame_size()
        with self.assertRaises(mp3.Mp3Error):
            mp3.parse_mp3_stream(frame(sz - 4)[:100])

    def test_bad_bitrate_index(self):
        with self.assertRaises(mp3.Mp3Error):
            mp3.parse_mp3_stream(frame(100, br_idx=15))

    def test_reserved_version(self):
        with self.assertRaises(mp3.Mp3Error):
            mp3.parse_mp3_stream(frame(100, version=1))

    def test_empty(self):
        with self.assertRaises(mp3.Mp3Error):
            mp3.parse_mp3_stream(b"")

    def test_bmu(self):
        sz = frame_size()
        stream = frame(sz - 4) * 2
        blob = mp3.BMU_MAGIC + struct.pack("<I", 8 + len(stream)) + stream
        b = mp3.parse_bmu(blob)
        self.assertEqual(b.declared_size, len(blob))
        self.assertEqual(len(b.stream.frames), 2)

    def test_bmu_size_mismatch(self):
        sz = frame_size()
        blob = mp3.BMU_MAGIC + struct.pack("<I", 999) + frame(sz - 4)
        with self.assertRaises(mp3.Mp3Error):
            mp3.parse_bmu(blob)

    def test_bmu_bad_magic(self):
        with self.assertRaises(mp3.Mp3Error):
            mp3.parse_bmu(b"XXXX" + bytes(100))


if __name__ == "__main__":
    unittest.main()
