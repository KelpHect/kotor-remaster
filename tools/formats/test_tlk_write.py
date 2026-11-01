#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/tlk_write.py (TLK writer).
No game data."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import tlk_write


def entry(flags, sound, vol, pitch, off, length, slen=0.0):
    return tlk_write.TlkEntry(flags, sound, vol, pitch, off, length, slen)


def build(entries, blob, language=0, text_offset=None) -> bytes:
    if text_offset is None:
        text_offset = tlk_write.HEADER.size + len(entries) * tlk_write.ENTRY.size + 8
    out = bytearray(tlk_write.HEADER.pack(tlk_write.MAGIC, tlk_write.VERSION,
                                          language, len(entries), text_offset))
    for e in entries:
        out += tlk_write._entry_bytes(e)
    out += b"\x00" * (text_offset - len(out))
    out += blob
    return bytes(out)


class TestTlkWrite(unittest.TestCase):
    def test_roundtrip_byte_identical(self):
        blob = b"helloworld!"
        e0 = entry(1, b"snd1".ljust(16, b"\x00"), 10, 20, 0, 5, 1.5)
        e1 = entry(0, b"".ljust(16, b"\x00"), 0, 0, 5, 6, 0.0)
        data = build([e0, e1], blob)
        t = tlk_write.parse_tlk_bytes(data)
        self.assertEqual(t.count, 2)
        self.assertEqual(t.text(0), b"hello")
        self.assertEqual(t.text(1), b"world!")
        self.assertEqual(tlk_write.tlk_bytes(t), data)

    def test_repack_semantic(self):
        e0 = entry(1, b"a".ljust(16, b"\x00"), 1, 2, 0, 3)
        e1 = entry(0, b"b".ljust(16, b"\x00"), 3, 4, 3, 2)
        data = build([e0, e1], b"xyzab")
        t = tlk_write.parse_tlk_bytes(data)
        repacked = tlk_write.tlk_repack(t)
        t2 = tlk_write.parse_tlk_bytes(repacked)
        self.assertEqual([t.text(i) for i in range(2)], [t2.text(i) for i in range(2)])
        self.assertEqual([e.flags for e in t.entries], [e.flags for e in t2.entries])
        # packed: no gap between table and blob
        self.assertEqual(t2.text_offset,
                         tlk_write.HEADER.size + 2 * tlk_write.ENTRY.size)

    def test_repack_new_entry_append(self):
        e0 = entry(0, b"".ljust(16, b"\x00"), 0, 0, 0, 2)
        data = build([e0], b"hi")
        t = tlk_write.parse_tlk_bytes(data)
        new = tlk_write.TlkEntry(0, b"".ljust(16, b"\x00"), 0, 0, 0, 3, 0.0)
        t.entries = t.entries + (new,)
        repacked = tlk_write.tlk_repack(t, texts=[t.text(0), b"bye"])
        t2 = tlk_write.parse_tlk_bytes(repacked)
        self.assertEqual(t2.count, 2)
        self.assertEqual(t2.text(1), b"bye")

    def test_overlap_rejected(self):
        e0 = entry(0, b"".ljust(16, b"\x00"), 0, 0, 0, 10)
        e1 = entry(0, b"".ljust(16, b"\x00"), 0, 0, 5, 10)
        data = build([e0, e1], b"x" * 20)
        with self.assertRaises(tlk_write.TlkError):
            tlk_write.parse_tlk_bytes(data)

    def test_text_out_of_blob(self):
        e0 = entry(0, b"".ljust(16, b"\x00"), 0, 0, 0, 999)
        data = build([e0], b"hi")
        with self.assertRaises(tlk_write.TlkError):
            tlk_write.parse_tlk_bytes(data)

    def test_bad_magic(self):
        with self.assertRaises(tlk_write.TlkError):
            tlk_write.parse_tlk_bytes(b"XLK V3.0" + bytes(100))

    def test_table_overrun(self):
        data = bytearray(tlk_write.HEADER.pack(tlk_write.MAGIC, tlk_write.VERSION,
                                               0, 1000, 20))
        with self.assertRaises(tlk_write.TlkError):
            tlk_write.parse_tlk_bytes(bytes(data))


if __name__ == "__main__":
    unittest.main()
