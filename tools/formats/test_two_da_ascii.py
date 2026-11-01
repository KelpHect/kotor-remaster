#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/two_da_ascii.py (2DA writers).

No game data: tables are built as two_da.TwoDA models directly."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import two_da
import two_da_ascii


def make_table(rows, columns=("label", "a", "b"), default=None):
    return two_da.TwoDA(
        version=two_da.VERSION_V2B,
        columns=tuple(columns),
        rows=tuple(two_da.Row(label=lab, cells=tuple(
            two_da.Cell(text=t) for t in cells)) for lab, cells in rows),
        default=default,
    )


class TestWriteV2b(unittest.TestCase):
    def test_roundtrip_byte_identical(self):
        t = make_table([("0", ("0", "x", "")), ("1", ("1", "y", "z"))])
        blob = two_da_ascii.write_2da_v2b(t)
        t2 = two_da.parse_2da(blob)
        self.assertEqual(t2.columns, t.columns)
        self.assertEqual(t2.rows, t.rows)
        # second encode is a fixed point (byte-identical re-encode)
        self.assertEqual(two_da_ascii.write_2da_v2b(t2), blob)

    def test_dedup_string_table(self):
        t = make_table([("0", ("0", "same", "same")), ("1", ("1", "same", ""))])
        blob = two_da_ascii.write_2da_v2b(t)
        t2 = two_da.parse_2da(blob)
        # both "same" cells share one offset
        lay = t2.layout
        self.assertLess(lay["string_table_size"], 20)
        self.assertEqual(t2.rows[0].cells[1].text, "same")

    def test_null_cell_written_as_empty(self):
        t = two_da.TwoDA(
            version=two_da.VERSION_V2A,
            columns=("label", "a"),
            rows=(two_da.Row("0", (two_da.Cell("0"), two_da.Cell(text="****", null=True))),),
        )
        blob = two_da_ascii.write_2da_v2b(t)
        t2 = two_da.parse_2da(blob)
        self.assertEqual(t2.rows[0].cells[1].text, "")
        self.assertFalse(t2.rows[0].cells[1].null)

    def test_zero_rows(self):
        t = make_table([], columns=("label",))
        blob = two_da_ascii.write_2da_v2b(t)
        t2 = two_da.parse_2da(blob)
        self.assertEqual(t2.row_count, 0)
        self.assertEqual(two_da_ascii.write_2da_v2b(t2), blob)

    def test_bad_column(self):
        t = make_table([("0", ("x",))], columns=("bad\tcol",))
        with self.assertRaises(two_da.TwoDAError):
            two_da_ascii.write_2da_v2b(t)


class TestWriteV2a(unittest.TestCase):
    def test_ascii_roundtrip_semantic(self):
        t = two_da.TwoDA(
            version=two_da.VERSION_V2A,
            columns=("label", "a b", "c"),
            rows=(two_da.Row("0", (two_da.Cell("x y"), two_da.Cell(""), two_da.Cell("q"))),
                  two_da.Row("1", (two_da.Cell("****", null=True), two_da.Cell("z"),
                                   two_da.Cell("w")))),
            default="0",
        )
        blob = two_da_ascii.write_2da_v2a(t)
        self.assertTrue(blob.startswith(b"2DA V2.0\r\nDefault: 0\r\n"))
        t2 = two_da.parse_2da(blob)
        self.assertEqual(t2.columns, t.columns)
        self.assertEqual(t2.rows, t.rows)
        self.assertEqual(t2.default, "0")

    def test_quoting(self):
        t = two_da.TwoDA(
            version=two_da.VERSION_V2A,
            columns=("label", "a"),
            rows=(two_da.Row("r 1", (two_da.Cell("r 1"), two_da.Cell("v"))),),
        )
        blob = two_da_ascii.write_2da_v2a(t)
        t2 = two_da.parse_2da(blob)
        self.assertEqual(t2.rows[0].label, "r 1")

    def test_convert_to_ascii_from_v2b(self):
        t = make_table([("0", ("0", "x", "y"))])
        blob = two_da_ascii.write_2da_v2b(t)
        ascii_blob = two_da_ascii.convert_to_ascii(blob)
        t2 = two_da.parse_2da(ascii_blob)
        self.assertEqual(t2.version, two_da.VERSION_V2A)
        self.assertEqual(t2.rows, t.rows)


class TestNulVariant(unittest.TestCase):
    def test_variant_roundtrip_byte_identical(self):
        import struct
        cols = ("label", "a", "b")
        labels = ["0", "1"]
        cells = [("0", "x", ""), ("1", "y", "z")]
        strings = bytearray()
        offs = {}
        off_list = []
        NUL = bytes([0])
        for row in cells:
            for c in row:
                raw = c.encode()
                if raw not in offs:
                    offs[raw] = len(strings)
                    strings += raw + NUL
                off_list.append(offs[raw])
        blob = bytearray(b"2DA V2.b" + bytes([10]))
        for c in cols:
            blob += c.encode() + NUL
        blob += NUL  # rim variant: one extra NUL terminates the column blob
        blob += struct.pack("<I", len(labels))
        for l in labels:
            blob += l.encode() + NUL
        for o in off_list:
            blob += struct.pack("<H", o)
        blob += struct.pack("<H", len(strings)) + strings
        tbl, layout = two_da_ascii.parse_2da_any(bytes(blob))
        self.assertEqual(layout, "v2b-nul")
        self.assertEqual(tbl.columns, cols)
        self.assertEqual([r.label for r in tbl.rows], labels)
        self.assertEqual(two_da_ascii.write_2da_v2b_nul(tbl), bytes(blob))

    def test_tab_layout_still_preferred(self):
        t = make_table([("0", ("0", "x", "y"))])
        blob = two_da_ascii.write_2da_v2b(t)
        _tbl, layout = two_da_ascii.parse_2da_any(blob)
        self.assertEqual(layout, "v2b-tab")


if __name__ == "__main__":
    unittest.main()
