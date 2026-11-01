#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/lip.py. No game data."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import lip


def build(frames, duration=None, magic=lip.MAGIC) -> bytes:
    if duration is None:
        duration = frames[-1][0] if frames else 0.0
    out = bytearray(magic)
    out += struct.pack("<fI", duration, len(frames))
    for t, v in frames:
        out += struct.pack("<fB", t, v)
    return bytes(out)


class TestLip(unittest.TestCase):
    def test_valid(self):
        l = lip.parse_lip(build([(0.0, 0), (0.5, 3), (1.0, 0)]))
        self.assertEqual(l.duration, 1.0)
        self.assertEqual(len(l.frames), 3)
        self.assertEqual(l.frames[1].viseme, 3)

    def test_zero_frames(self):
        l = lip.parse_lip(build([], duration=0.0))
        self.assertEqual(len(l.frames), 0)

    def test_bad_magic(self):
        with self.assertRaises(lip.LipError):
            lip.parse_lip(build([(0.0, 0)], magic=b"LIP V0.9"))

    def test_size_mismatch(self):
        with self.assertRaises(lip.LipError):
            lip.parse_lip(build([(0.0, 0)]) + b"\x00")

    def test_viseme_range(self):
        with self.assertRaises(lip.LipError):
            lip.parse_lip(build([(0.0, 16), (1.0, 0)]))

    def test_non_monotonic(self):
        with self.assertRaises(lip.LipError):
            lip.parse_lip(build([(0.0, 0), (0.5, 1), (0.2, 2), (1.0, 0)]))

    def test_time_outside_duration(self):
        with self.assertRaises(lip.LipError):
            lip.parse_lip(build([(0.0, 0), (2.0, 0)], duration=1.0))

    def test_last_must_equal_duration(self):
        with self.assertRaises(lip.LipError):
            lip.parse_lip(build([(0.0, 0), (0.9, 0)], duration=1.0))

    def test_first_must_be_zero(self):
        with self.assertRaises(lip.LipError):
            lip.parse_lip(build([(0.1, 0), (1.0, 0)]))


if __name__ == "__main__":
    unittest.main()
