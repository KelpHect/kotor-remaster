#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/wok.py. No game data."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import wok
import bwm
from test_bwm import build_bwm


class TestWok(unittest.TestCase):
    def test_parse_wok(self):
        w = wok.parse_wok(build_bwm())
        self.assertEqual(w.type, 1)
        st = wok.winding_stats(w)
        # face (0,1,0) is degenerate; use a real triangle instead
        self.assertEqual(st.winding_negative + st.winding_positive
                         + st.winding_zero, 1)

    def test_winding_rh(self):
        # CCW triangle in XY with +Z normal => positive dot (RH Z-up)
        data = build_bwm(vc=3, faces=((0, 1, 2),))
        # set verts (0,0,0),(1,0,0),(0,1,0) and normal (0,0,1)
        import struct
        data = bytearray(data)
        struct.pack_into("<3f", data, 0x88 + 12, 1.0, 0.0, 0.0)
        struct.pack_into("<3f", data, 0x88 + 24, 0.0, 1.0, 0.0)
        w = wok.parse_wok(bytes(data))
        st = wok.winding_stats(w)
        self.assertEqual(st.winding_positive, 1)
        self.assertGreater(st.min_normal_dot, 0)

    def test_rejects_type0(self):
        with self.assertRaises(bwm.BwmError):
            wok.parse_wok(build_bwm(btype=0, normals=False))


if __name__ == "__main__":
    unittest.main()
