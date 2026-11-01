#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/vis.py. No game data."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import vis


def build(sections, crlf=True) -> bytes:
    nl = "\r\n" if crlf else "\n"
    out = ""
    for room, entries, count in sections:
        c = len(entries) if count is None else count
        out += f"{room} {c}{nl}" if count is not False else f"{room}{nl}"
        for v in entries:
            out += f"  {v}{nl}"
    return out.encode("latin-1")


class TestVis(unittest.TestCase):
    def test_basic(self):
        v = vis.parse_vis(build([("a", ["b", "c"], None), ("b", ["a"], None), ("c", [], None)]))
        self.assertEqual(len(v.entries), 3)
        self.assertEqual(v.visible_from("a"), ("b", "c"))
        self.assertEqual(v.visible_from("A"), ("b", "c"))  # case-insensitive lookup
        self.assertEqual(v.warnings, [])

    def test_lf(self):
        v = vis.parse_vis(build([("a", ["a"], None)], crlf=False))
        self.assertEqual(v.visible_from("a"), ("a",))

    def test_count_mismatch_is_warning(self):
        v = vis.parse_vis(build([("a", ["a"], 6)]))
        self.assertEqual(len(v.warnings), 1)
        self.assertIn("declares 6", v.warnings[0])

    def test_bare_header_allowed(self):
        v = vis.parse_vis(b"a\n  b\n")
        self.assertEqual(v.entries[0].count, None)
        self.assertEqual(v.entries[0].entries, ("b",))

    def test_unknown_room_entry_is_warning(self):
        v = vis.parse_vis(build([("a", ["zzz"], None)]))
        self.assertTrue(any("unknown room" in w for w in v.warnings))

    def test_duplicate_section_is_warning(self):
        v = vis.parse_vis(build([("a", [], None), ("A", [], None)]))
        self.assertTrue(any("duplicate" in w for w in v.warnings))

    def test_entry_before_header(self):
        with self.assertRaises(vis.VisError):
            vis.parse_vis(b"  a\n")

    def test_multi_token_entry(self):
        with self.assertRaises(vis.VisError):
            vis.parse_vis(b"a 1\n  b c\n")

    def test_bad_header(self):
        with self.assertRaises(vis.VisError):
            vis.parse_vis(b"a b c\n")

    def test_empty(self):
        with self.assertRaises(vis.VisError):
            vis.parse_vis(b"")

    def test_nul(self):
        with self.assertRaises(vis.VisError):
            vis.parse_vis(b"a 0\x00\n")


if __name__ == "__main__":
    unittest.main()
