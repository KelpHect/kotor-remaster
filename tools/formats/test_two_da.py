#!/usr/bin/env python3
"""Synthetic-byte tests for the lane 02 2DA parser.

No game data is used.  Run with:
    python -m unittest -v artifacts/lanes/02-2da/test_two_da.py
or from this directory:
    python -m unittest -v test_two_da
"""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import two_da


def build_v2b(
    columns,
    labels,
    rows,
    *,
    newline: bytes = b"\n",
    offsets=None,
    string_size=None,
    trailing: bytes = b"",
    column_blob=None,
    row_count=None,
) -> bytes:
    """Build a V2.b resource from strings (cells are text, "" is empty)."""
    out = bytearray(b"2DA V2.b" + newline)
    if column_blob is None:
        for column in columns:
            out += column.encode("cp1252") + b"\t"
    else:
        out += column_blob
    out += b"\x00"
    out += struct.pack("<I", len(labels) if row_count is None else row_count)
    for label in labels:
        out += label.encode("cp1252") + b"\t"

    strings = bytearray()
    wanted: dict[str, int] = {}
    computed_offsets: list[int] = []
    for row in rows:
        for value in row:
            if value not in wanted:
                wanted[value] = len(strings)
                strings += value.encode("cp1252") + b"\x00"
            computed_offsets.append(wanted[value])
    if offsets is None:
        offsets = computed_offsets
    for value in offsets:
        out += struct.pack("<H", value)
    out += struct.pack(
        "<H", len(strings) if string_size is None else string_size)
    out += strings + trailing
    return bytes(out)


class BinaryV2BTests(unittest.TestCase):
    def test_basic_layout_and_empty_vs_literal_marker(self):
        data = build_v2b(
            ["a", "b", "c"],
            ["0", "1"],
            [["", "x", "****"], ["y", "", "z"]],
        )
        table = two_da.parse_2da(data, "synthetic")
        self.assertEqual(table.version, "V2.b")
        self.assertEqual(table.column_count, 3)
        self.assertEqual(table.row_count, 2)
        self.assertEqual(table.columns, ("a", "b", "c"))
        self.assertEqual([row.label for row in table.rows], ["0", "1"])
        self.assertEqual(table.counts(), {
            "rows": 2, "columns": 3, "cells": 6,
            "empty_cells": 2, "null_cells": 0,
        })
        self.assertEqual(table.cell(0, 0), two_da.Cell("", False))
        self.assertTrue(table.cell(0, 0).is_empty)
        self.assertEqual(table.cell(0, 2).text, "****")
        self.assertFalse(table.cell(0, 2).null)
        self.assertEqual(table.cell("1", "a").text, "y")
        self.assertEqual(table.layout["cell_offset_table_size"], 12)
        self.assertEqual(table.layout["string_table_size"], len(
            b"\x00x\x00****\x00y\x00z\x00"))
        self.assertEqual(table.layout["unreferenced_string_bytes"], 0)
        self.assertFalse(table.warnings)

    def test_zero_rows_zero_columns(self):
        data = build_v2b([], [], [])
        table = two_da.parse_2da(data)
        self.assertEqual(table.counts()["rows"], 0)
        self.assertEqual(table.counts()["columns"], 0)
        self.assertEqual(table.counts()["cells"], 0)

    def test_crlf_header(self):
        table = two_da.parse_2da(build_v2b(["a"], ["0"], [["v"]], newline=b"\r\n"))
        self.assertEqual(table.cell(0, 0).text, "v")

    def test_short_header(self):
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(b"2DA V2.b", "short")
        self.assertIn("short V2.b header", ctx.exception.reason)
        self.assertEqual(ctx.exception.table, "short")

    def test_bad_signature(self):
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(b"2DA V9.b\n\x00")
        self.assertIn("unsupported", ctx.exception.reason)

    def test_unterminated_column_blob(self):
        data = b"2DA V2.b\nabc"
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("unterminated column", ctx.exception.reason)
        self.assertEqual(ctx.exception.offset, 9)

    def test_column_not_tab_terminated(self):
        data = b"2DA V2.b\nabc\x00" + struct.pack("<I", 0) + b"\x00\x00"
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("TAB-terminated", ctx.exception.reason)

    def test_empty_column_name(self):
        data = build_v2b([], ["0"], [["x"]], column_blob=b"\t\x00")
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("empty column name", ctx.exception.reason)

    def test_rows_without_columns(self):
        data = build_v2b([], ["0"], [])
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("rows present but no columns", ctx.exception.reason)

    def test_truncated_row_count(self):
        data = b"2DA V2.b\na\t\x00\x01\x00"
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("truncated row count", ctx.exception.reason)

    def test_unterminated_row_label(self):
        data = b"2DA V2.b\na\t\x00" + struct.pack("<I", 1) + b"0"
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("unterminated row label", ctx.exception.reason)

    def test_implausible_row_count(self):
        data = b"2DA V2.b\na\t\x00" + struct.pack("<I", two_da.MAX_ROWS + 1)
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("implausible row count", ctx.exception.reason)

    def test_truncated_offset_table(self):
        data = b"2DA V2.b\na\t\x00" + struct.pack("<I", 2) + b"0\t1\t" + b"\x00\x00\x00"
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("truncated cell-offset table", ctx.exception.reason)
        self.assertEqual(ctx.exception.offset, 20)

    def test_offset_overflow(self):
        data = build_v2b(["a"], ["0"], [["hello"]], offsets=[0xFFFF], string_size=6)
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("outside string table", ctx.exception.reason)
        self.assertEqual(ctx.exception.offset, 18)  # offset-table entry 0
        self.assertEqual(ctx.exception.table, None)

    def test_offset_to_unterminated_string(self):
        # 9 header + 2 column + 1 NUL + 4 row count + 2 label = 18
        data = (b"2DA V2.b\na\t\x00" + struct.pack("<I", 1) + b"0\t"
                + struct.pack("<HH", 0, 1) + b"x")
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("unterminated string", ctx.exception.reason)

    def test_string_table_overflows_file(self):
        data = build_v2b(["a"], ["0"], [["x"]], string_size=99)
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("exceeds file", ctx.exception.reason)

    def test_trailing_bytes_warn(self):
        table = two_da.parse_2da(
            build_v2b(["a"], ["0"], [["x"]], trailing=b"JUNK"))
        self.assertTrue(any("trailing byte" in warning for warning in table.warnings))

    def test_duplicate_labels_warn_not_fail(self):
        table = two_da.parse_2da(build_v2b(["a"], ["0", "0"], [["x"], ["y"]]))
        self.assertTrue(any("duplicate row label" in warning for warning in table.warnings))
        self.assertEqual(table.row_index("0"), 0)

    def test_duplicate_columns_warn(self):
        table = two_da.parse_2da(build_v2b(["a", "a"], ["0"], [["x", "y"]]))
        self.assertTrue(any("duplicate column" in warning for warning in table.warnings))
        self.assertEqual(table.column_index("a"), 0)

    def test_cell_access_errors(self):
        table = two_da.parse_2da(build_v2b(["a"], ["0"], [["x"]]))
        with self.assertRaises(KeyError):
            table.cell(0, "nope")
        with self.assertRaises(IndexError):
            table.cell(5, 0)


class AsciiV2ATests(unittest.TestCase):
    def test_basic_quotes_default_and_nulls(self):
        data = (
            b"2DA V2.0\r\n"
            b"\r\n"
            b"Default: 7\r\n"
            b"   a       b             c\r\n"
            b"0  x       \"hello world\" ****\r\n"
            b"1  \"\"      y\r\n"
        )
        table = two_da.parse_2da(data, "ascii")
        self.assertEqual(table.version, "V2.0")
        self.assertEqual(table.columns, ("a", "b", "c"))
        self.assertEqual(table.default, "7")
        self.assertEqual(table.counts(), {
            "rows": 2, "columns": 3, "cells": 6,
            "empty_cells": 1, "null_cells": 2,
        })
        self.assertEqual(table.cell(0, 1).text, "hello world")
        self.assertTrue(table.cell(0, 2).null)
        self.assertTrue(table.cell(1, 0).is_empty)
        self.assertTrue(table.cell(1, 2).null)  # missing trailing cell
        self.assertTrue(any("null-filled" in warning for warning in table.warnings))

    def test_quoted_marker_is_still_null(self):
        data = b'2DA V2.0\n\n a\n0 "****"\n'
        table = two_da.parse_2da(data)
        self.assertTrue(table.cell(0, 0).null)
        self.assertEqual(table.cell(0, 0).as_text(), "****")

    def test_bad_header(self):
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(b"2DA V2.1\n")
        self.assertIn("unsupported", ctx.exception.reason)

    def test_no_columns(self):
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(b"2DA V2.0\n\n")
        self.assertIn("no column header", ctx.exception.reason)

    def test_extra_cells(self):
        data = b"2DA V2.0\n\n a b\n0 x y z\n"
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("cells for 2 columns", ctx.exception.reason)

    def test_unterminated_quote(self):
        data = b'2DA V2.0\n\n a\n0 "oops\n'
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("unterminated quoted cell", ctx.exception.reason)

    def test_chars_after_quote(self):
        data = b'2DA V2.0\n\n a\n0 "oops"tail\n'
        with self.assertRaises(two_da.TwoDAError) as ctx:
            two_da.parse_2da(data)
        self.assertIn("after closing quote", ctx.exception.reason)

    def test_empty_table(self):
        table = two_da.parse_2da(b"2DA V2.0\n\n a\n")
        self.assertEqual(table.counts()["rows"], 0)
        self.assertEqual(table.counts()["columns"], 1)

    def test_duplicate_label_warns(self):
        table = two_da.parse_2da(b"2DA V2.0\n\n a\n0 x\n0 y\n")
        self.assertEqual(table.cell(1, 0).text, "y")
        self.assertTrue(any("duplicate row label" in warning for warning in table.warnings))


if __name__ == "__main__":
    unittest.main()
