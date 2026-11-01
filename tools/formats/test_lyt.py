#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/lyt.py. No game data."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import lyt


def build_lyt(rooms=(("r01", 1.0, 2.0, 0.0),),
              tracks=(), obstacles=(), hooks=(),
              header=lyt.HEADER, dep="x.max", extra=b"", crlf=True) -> bytes:
    nl = "\r\n" if crlf else "\n"
    out = header + nl
    if dep is not None:
        out += f"filedependancy {dep}" + nl
    out += "beginlayout" + nl
    out += f"roomcount {len(rooms)}" + nl
    for r in rooms:
        out += f"   {r[0]} {r[1]} {r[2]} {r[3]}" + nl
    out += f"trackcount {len(tracks)}" + nl
    for r in tracks:
        out += f"   {r[0]} {r[1]} {r[2]} {r[3]}" + nl
    out += f"obstaclecount {len(obstacles)}" + nl
    for r in obstacles:
        out += f"   {r[0]} {r[1]} {r[2]} {r[3]}" + nl
    out += f"doorhookcount {len(hooks)}" + nl
    for h in hooks:
        out += "   " + " ".join(str(x) for x in h) + nl
    out += "donelayout" + nl
    return out.encode("latin-1") + extra


HOOK = ("r01", "Door_01", 0, 1.5, 2.5, -0.04, 1.0, 0.0, 0.0, 0.0)


class TestLyt(unittest.TestCase):
    def test_full(self):
        data = build_lyt(rooms=(("r01", 1, 2, 0), ("r02", 3, 4, 0)),
                         tracks=(("t01", 9, 9, 0),),
                         obstacles=(("o01", 5, 6, 0),),
                         hooks=(HOOK,))
        l = lyt.parse_lyt(data)
        self.assertEqual(len(l.rooms), 2)
        self.assertEqual(l.rooms[0].name, "r01")
        self.assertEqual(l.rooms[0].x, 1.0)
        self.assertEqual(len(l.tracks), 1)
        self.assertEqual(len(l.obstacles), 1)
        self.assertEqual(len(l.doorhooks), 1)
        h = l.doorhooks[0]
        self.assertEqual(h.quat, (1.0, 0.0, 0.0, 0.0))
        self.assertEqual(h.node, "Door_01")
        self.assertEqual(l.filedependancy, "x.max")

    def test_lf_only(self):
        data = build_lyt(crlf=False)
        l = lyt.parse_lyt(data)
        self.assertEqual(len(l.rooms), 1)

    def test_comments_and_blank_lines(self):
        data = build_lyt() .replace(b"beginlayout", b"# comment\n\nbeginlayout")
        l = lyt.parse_lyt(data)
        self.assertEqual(len(l.rooms), 1)

    def test_bad_header(self):
        with self.assertRaises(lyt.LytError):
            lyt.parse_lyt(b"#MAXLAYOT ASCII\nbeginlayout\n")

    def test_count_mismatch(self):
        data = build_lyt().replace(b"roomcount 1", b"roomcount 2")
        with self.assertRaises(lyt.LytError):
            lyt.parse_lyt(data)

    def test_trailing_line_is_warning(self):
        # shipped quirk (m12aa): a stray line after donelayout
        l = lyt.parse_lyt(build_lyt(extra=b"M12aa_01m" + bytes([10])))
        self.assertTrue(any("trailing" in w for w in l.warnings))

    def test_hook_unknown_room_is_warning(self):
        # shipped quirk: stunt LYTs reference rooms from the real layout
        l = lyt.parse_lyt(build_lyt(hooks=(("nope", "Door_01", 0, 0, 0, 0, 1, 0, 0, 0),)))
        self.assertTrue(any("unknown room" in w for w in l.warnings))

    def test_bad_float(self):
        data = build_lyt(rooms=(("r01", "x", 2, 0),))
        with self.assertRaises(lyt.LytError):
            lyt.parse_lyt(data)

    def test_hook_wrong_token_count(self):
        data = build_lyt(hooks=(("r01", "Door_01", 0, 1, 2, 3, 1, 0, 0),))
        with self.assertRaises(lyt.LytError):
            lyt.parse_lyt(data)

    def test_duplicate_room_is_warning(self):
        # shipped quirk: stunt LYTs repeat the placeholder name '****'
        l = lyt.parse_lyt(build_lyt(rooms=(("****", 1, 2, 0), ("****", 3, 4, 0))))
        self.assertTrue(any("duplicate" in w for w in l.warnings))

    def test_nul_rejected(self):
        with self.assertRaises(lyt.LytError):
            lyt.parse_lyt(b"#MAXLAYOUT ASCII\x00\n")


if __name__ == "__main__":
    unittest.main()
