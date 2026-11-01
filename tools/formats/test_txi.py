#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/txi.py. No game data."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import txi


class TestTxi(unittest.TestCase):
    def test_parse_write_roundtrip(self):
        blob = b"islightmap 1\r\ncompresstexture 0\r\nmipmap 0\r\ndownsamplemax 0\r\n"
        t = txi.parse_txi(blob)
        self.assertEqual(t["islightmap"], "1")
        self.assertEqual(t["downsamplemax"], "0")
        self.assertEqual(txi.write_txi(t), blob)

    def test_lf(self):
        t = txi.parse_txi(b"mipmap 0\n")
        self.assertEqual(t["mipmap"], "0")

    def test_punct_key_allowed(self):
        # shipped TXIs contain numeric keys like "0.02"
        t = txi.parse_txi(b"0.02 x\n")
        self.assertEqual(t["0.02"], "x")

    def test_duplicate_key_is_warning(self):
        # shipped quirk: a few TXIs repeat a key
        t = txi.parse_txi(b"mipmap 0\nmipmap 1\n")
        self.assertEqual(t["mipmap"], "0")
        self.assertEqual(len(t.warnings), 1)

    def test_nul(self):
        with self.assertRaises(txi.TxiError):
            txi.parse_txi(b"mipmap\x00 0\n")

    def test_empty(self):
        self.assertEqual(dict(txi.parse_txi(b"")), {})

    def test_blank_lines_allowed(self):
        t = txi.parse_txi(b"mipmap 0\r\n\r\ndownsamplemax 0\r\n")
        self.assertEqual(list(t), ["mipmap", "downsamplemax"])

    def test_uppercase_keys(self):
        t = txi.parse_txi(b"Decal 1\r\nspacingR 2\r\n")
        self.assertEqual(t["Decal"], "1")
        self.assertEqual(t["spacingR"], "2")

    def test_empty_value(self):
        t = txi.parse_txi(b"filter\n")
        self.assertEqual(t["filter"], "")

    def test_write_rejects_newline_in_value(self):
        with self.assertRaises(txi.TxiError):
            txi.write_txi([("mipmap", "0\n1")])


if __name__ == "__main__":
    unittest.main()
