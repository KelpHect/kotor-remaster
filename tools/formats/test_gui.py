#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/gui.py. No game data:
GUI fixtures are GFF files built with tools/resources/kotor_gff_write.py."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "resources"))

import gui  # noqa: E402
import kotor_gff_write as gw  # noqa: E402


def control(ctype=1, tag="btn", children=()) -> gw.GffStruct:
    c = gw.GffStruct(0)
    c.add("CONTROLTYPE", "int32", ctype)
    c.add("TAG", "string", tag)
    c.add("CONTROLS", "list", list(children))
    c.add("ALPHA", "float", 1.0)
    return c


def build_gui() -> bytes:
    root = gw.GffStruct(0xFFFFFFFF)
    root.add("CONTROLTYPE", "int32", 0)
    root.add("TAG", "string", "screen")
    root.add("Obj_Locked", "uint8", 0)
    root.add("EXTENT", "struct", _extent())
    root.add("CONTROLS", "list", [control(1, "btn1"), control(2, "lbl", [control(3, "sub")])])
    return gw.gff_bytes(b"GUI ", root, b"V3.2")


def _extent() -> gw.GffStruct:
    e = gw.GffStruct(0)
    e.add("left", "int32", 0).add("top", "int32", 0)
    e.add("width", "int32", 640).add("height", "int32", 480)
    return e


class TestGui(unittest.TestCase):
    def test_valid_tree(self):
        g = gui.parse_gui(build_gui())
        self.assertEqual(g.tag, "screen")
        self.assertEqual(gui.count_controls(g.root), 4)
        self.assertEqual(g.root.children[1].children[0].tag, "sub")

    def test_bad_magic(self):
        root = gw.GffStruct(0)
        data = gw.gff_bytes(b"ARE ", root, b"V3.2")
        with self.assertRaises(gui.GuiError):
            gui.parse_gui(data)

    def test_missing_controls(self):
        root = gw.GffStruct(0xFFFFFFFF)
        root.add("CONTROLTYPE", "int32", 0)
        data = gw.gff_bytes(b"GUI ", root, b"V3.2")
        with self.assertRaises(gui.GuiError):
            gui.parse_gui(data)

    def test_control_missing_controltype(self):
        bad = gw.GffStruct(0)
        bad.add("TAG", "string", "x")
        bad.add("CONTROLS", "list", [])
        root = gw.GffStruct(0xFFFFFFFF)
        root.add("CONTROLTYPE", "int32", 0)
        root.add("CONTROLS", "list", [bad])
        data = gw.gff_bytes(b"GUI ", root, b"V3.2")
        with self.assertRaises(gui.GuiError):
            gui.parse_gui(data)

    def test_deep_tree_rejected(self):
        node = control(1, "leaf")
        for _ in range(70):
            node = control(1, "n", [node])
        root = gw.GffStruct(0xFFFFFFFF)
        root.add("CONTROLTYPE", "int32", 0)
        root.add("CONTROLS", "list", [node])
        data = gw.gff_bytes(b"GUI ", root, b"V3.2")
        with self.assertRaises(gui.GuiError):
            gui.parse_gui(data)


if __name__ == "__main__":
    unittest.main()
