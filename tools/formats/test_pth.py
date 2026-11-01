#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/pth.py (GFF PTH + gff_reencode).

No game data: PTH/GFF fixtures are built with the project writer
tools/resources/kotor_gff_write.py.
"""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "resources"))

import gff  # noqa: E402
import pth  # noqa: E402
import kotor_gff_write as gw  # noqa: E402


def build_pth_gff(points, connections) -> bytes:
    root = gw.GffStruct(0xFFFFFFFF)
    pts = []
    for (x, y, first, cnt) in points:
        s = gw.GffStruct(0)
        s.add("X", "float", x).add("Y", "float", y)
        s.add("First_Conection", "uint32", first).add("Conections", "uint32", cnt)
        pts.append(s)
    cons = []
    for dest in connections:
        s = gw.GffStruct(0)
        s.add("Destination", "uint32", dest)
        cons.append(s)
    root.add("Path_Points", "list", pts)
    root.add("Path_Conections", "list", cons)
    return gw.gff_bytes(b"PTH ", root, b"V3.2")


class TestPth(unittest.TestCase):
    def test_valid(self):
        data = build_pth_gff([(1.0, 2.0, 0, 2), (3.0, 4.0, 2, 1)], [1, 0, 0])
        p = pth.parse_pth(data)
        self.assertEqual(len(p.points), 2)
        self.assertEqual(len(p.connections), 3)
        self.assertEqual(p.points[0].first_connection, 0)
        self.assertEqual(p.connections[1].destination, 0)

    def test_gff_reencode_byte_identical(self):
        data = build_pth_gff([(1.0, 2.0, 0, 1)], [0])
        g = pth.load_gff_bytes(data)
        self.assertEqual(pth.gff_reencode(g), data)

    def test_gff_reencode_real_shape(self):
        # struct with several field types incl. locstring and vector3
        def make_child():
            child = gw.GffStruct(100)
            child.add("Name", "locstring", {"strref": 42, "substrings": [(0, "hi")]})
            child.add("Pos", "vector3", (1.0, 2.0, 3.0))
            child.add("Bin", "binary", b"\x01\x02\x03\x04")
            child.add("Big", "uint64", 1 << 40)
            return child
        root = gw.GffStruct(0xFFFFFFFF)
        root.add("AreaProperties", "struct", make_child())
        root.add("Tags", "list", [make_child()])
        data = gw.gff_bytes(b"GIT ", root, b"V3.2")
        g = pth.load_gff_bytes(data)
        self.assertEqual(gff.check(g), [])
        self.assertEqual(pth.gff_reencode(g), data)

    def test_bad_magic(self):
        root = gw.GffStruct(0)
        data = gw.gff_bytes(b"XYZ ", root, b"V3.2")
        with self.assertRaises(pth.PthError):
            pth.parse_pth(data)

    def test_destination_out_of_range(self):
        data = build_pth_gff([(1.0, 2.0, 0, 1)], [5])
        with self.assertRaises(pth.PthError):
            pth.parse_pth(data)

    def test_connection_range_overrun(self):
        data = build_pth_gff([(1.0, 2.0, 0, 9)], [0])
        with self.assertRaises(pth.PthError):
            pth.parse_pth(data)

    def test_missing_lists(self):
        root = gw.GffStruct(0)
        data = gw.gff_bytes(b"PTH ", root, b"V3.2")
        with self.assertRaises(pth.PthError):
            pth.parse_pth(data)


if __name__ == "__main__":
    unittest.main()
