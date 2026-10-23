#!/usr/bin/env python3
"""Regression tests for tools/formats/gff.py (lane 03).

Run from this directory:  python -m unittest -v test_gff_types

All fixtures are generated in memory; no game data is used.
"""
from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gff  # noqa: E402


class Build:
    """Minimal KotOR GFF writer used only by tests; no game data."""

    def __init__(self, magic=b"TEST"):
        self.magic = magic
        self.labels, self._labels = [], {}
        self.fields, self.fdata, self.lidx = [], bytearray(), bytearray()
        self.structs, self.fidx = [], bytearray()
        self.struct_field_gap = 0
        self.slot_order = None
        self.slot_prefix = 0

    def label(self, name):
        if name not in self._labels:
            self._labels[name] = len(self.labels)
            self.labels.append(name)
        return self._labels[name]

    def field(self, ftype, label, data):
        self.fields.append((ftype, label, data))
        return len(self.fields) - 1

    def struct(self, type_id, rows):
        self.structs.append((type_id, rows))
        return len(self.structs) - 1

    def inline(self, value, size=4):
        return value & {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[size]

    def data(self, payload):
        off = len(self.fdata)
        self.fdata += payload
        return off

    def string(self, text):
        raw = text.encode("latin-1")
        return self.data(struct.pack("<I", len(raw)) + raw)

    def resref(self, text):
        raw = text.encode("latin-1")
        return self.data(bytes([len(raw)]) + raw)

    def locstring(self, strref, substrings, total=None):
        body = struct.pack("<II", strref, len(substrings))
        for lang, text in substrings:
            raw = text.encode("latin-1")
            body += struct.pack("<II", lang, len(raw)) + raw
        return self.data(struct.pack("<I", 8 + len(body) - 8 if total is None
                                     else total) + body)

    def binary(self, raw, length=None):
        return self.data(struct.pack("<I", len(raw) if length is None
                                     else length) + raw)

    def vector(self, values):
        return self.data(struct.pack("<%df" % len(values), *values))

    def list(self, indices):
        off = len(self.lidx)
        self.lidx += struct.pack("<I", len(indices))
        self.lidx += struct.pack("<%dI" % len(indices), *indices)
        return off

    def build(self):
        rows = [(t, self.label(name), d) for t, name, d in self.fields]
        order = self.slot_order if self.slot_order is not None else range(len(self.structs))
        cursor, slots = self.slot_prefix, {}
        self.fidx += b"\0" * (4 * self.slot_prefix)
        for si in order:
            _type, srows = self.structs[si]
            if len(srows) <= 1:
                continue
            self.fidx += struct.pack("<%dI" % len(srows), *srows)
            slots[si], cursor = cursor, cursor + len(srows)
        structs = bytearray()
        for si, (type_id, srows) in enumerate(self.structs):
            if not srows:
                off = 0
            elif len(srows) == 1:
                off = srows[0]
            else:
                off = slots[si] * 4
            structs += struct.pack("<3I", type_id, off, len(srows))
        fields = b"".join(struct.pack("<3I", *r) for r in rows)
        labels = b"".join(x.encode("latin-1")[:16].ljust(16, b"\0") for x in self.labels)
        so = 56
        fo = so + len(structs) + self.struct_field_gap
        lo = fo + len(fields)
        do = lo + len(labels)
        xo = do + len(self.fdata)
        io = xo + len(self.fidx)
        head = self.magic + b"V3.2" + struct.pack(
            "<12I", so, len(self.structs), fo, len(rows), lo, len(self.labels),
            do, len(self.fdata), xo, len(self.fidx), io, len(self.lidx))
        return (head + structs + b"\0" * self.struct_field_gap + fields + labels
                + self.fdata + self.fidx + self.lidx)


class TempFile:
    def __init__(self, blob):
        import tempfile
        fd, self.path = tempfile.mkstemp(suffix=".gff")
        os.write(fd, blob)
        os.close(fd)

    def __enter__(self):
        return self.path

    def __exit__(self, *exc):
        os.unlink(self.path)


def one_field(ftype, data, expected):
    b = Build()
    b.struct(0xFFFFFFFF, [0, 1])
    b.field(ftype, "Test", data)
    b.field(0, "Dummy", b.inline(1))
    return b, expected


class TypeTests(unittest.TestCase):
    def roundtrip(self, b, expected):
        with TempFile(b.build()) as path:
            g = gff.Gff(path)
            self.assertEqual(gff.check(g), [])
            self.assertEqual(gff.check_values(g), [])
            got = g.value(g.fields_of(0)[0])
            self.assertEqual(got, expected)
            return g

    def test_type_00_uint8(self):
        self.roundtrip(*one_field(0, 200, 200))

    def test_type_01_int8(self):
        self.roundtrip(*one_field(1, 0xFB, -5))

    def test_type_02_uint16(self):
        self.roundtrip(*one_field(2, 60000, 60000))

    def test_type_03_int16(self):
        self.roundtrip(*one_field(3, 0xCFC7, -12345))

    def test_type_04_uint32(self):
        self.roundtrip(*one_field(4, 0xDEADBEEF, 0xDEADBEEF))

    def test_type_05_int32(self):
        self.roundtrip(*one_field(5, (-123456789) & 0xFFFFFFFF, -123456789))

    def test_type_06_uint64(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(6, "Test", b.data(struct.pack("<Q", 0x1122334455667788)))
        b.field(0, "Dummy", b.inline(1))
        self.roundtrip(b, 0x1122334455667788)

    def test_type_07_int64(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(7, "Test", b.data(struct.pack("<q", -2)))
        b.field(0, "Dummy", b.inline(1))
        self.roundtrip(b, -2)

    def test_type_08_float(self):
        bits = struct.unpack("<I", struct.pack("<f", 1.5))[0]
        self.roundtrip(*one_field(8, bits, 1.5))

    def test_type_09_double(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(9, "Test", b.data(struct.pack("<d", 2.5)))
        b.field(0, "Dummy", b.inline(1))
        self.roundtrip(b, 2.5)

    def test_type_10_string_without_nul(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(10, "Test", b.string("Hello"))
        b.field(0, "Dummy", b.inline(1))
        self.roundtrip(b, "Hello")

    def test_type_11_resref(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(11, "Test", b.resref("abc"))
        b.field(0, "Dummy", b.inline(1))
        self.roundtrip(b, "abc")

    def test_type_12_locstring_total_is_8_plus_substrings(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(12, "Test", b.locstring(0, [(0, "Hello")]))
        b.field(0, "Dummy", b.inline(1))
        self.roundtrip(b, {"total": 21, "strref": 0,
                           "substrings": [(0, "Hello")]})

    def test_type_13_binary(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(13, "Test", b.binary(b"\x01\x02\x03\x04"))
        b.field(0, "Dummy", b.inline(1))
        self.roundtrip(b, b"\x01\x02\x03\x04")

    def test_type_14_struct(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.struct(0x64, [2])
        b.field(14, "Test", 1)
        b.field(0, "Dummy", b.inline(1))
        b.field(0, "Child", b.inline(7))
        self.roundtrip(b, {"__struct__": 1})

    def test_type_15_list(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.struct(9, [2])
        b.field(15, "Test", b.list([1]))
        b.field(0, "Dummy", b.inline(1))
        b.field(0, "Child", b.inline(7))
        self.roundtrip(b, {"__list__": 0})

    def test_type_16_vector4(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(16, "Test", b.vector((1.0, 2.0, 3.0, 4.0)))
        b.field(0, "Dummy", b.inline(1))
        self.roundtrip(b, (1.0, 2.0, 3.0, 4.0))

    def test_type_17_vector3(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(17, "Test", b.vector((1.0, 2.0, 3.0)))
        b.field(0, "Dummy", b.inline(1))
        self.roundtrip(b, (1.0, 2.0, 3.0))

    def test_type_18_strref(self):
        self.roundtrip(*one_field(18, 42, 42))


class StructureTests(unittest.TestCase):
    def test_root_written_last_slots(self):
        """Shipped KotOR layout: slot order is creation order, not struct order."""
        b = Build()
        b.struct(0xFFFFFFFF, [2, 3])
        b.struct(1, [0, 1])
        b.field(0, "RootA", b.inline(1))
        b.field(0, "RootB", b.inline(2))
        b.field(0, "ChildA", b.inline(3))
        b.field(0, "ChildB", b.inline(4))
        b.slot_order = [1, 0]
        with TempFile(b.build()) as path:
            g = gff.Gff(path)
            self.assertEqual(gff.check(g), [])

    def test_stale_slot_entries_are_allowed(self):
        """Field-index entries not referenced by any struct are shipped data."""
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(0, "A", b.inline(1))
        b.field(0, "B", b.inline(2))
        b.slot_prefix = 2   # two stale entries before the root's range
        with TempFile(b.build()) as path:
            g = gff.Gff(path)
            self.assertEqual(g.structs[0].slot, 2)
            self.assertEqual(gff.check(g), [])

    def test_duplicate_labels_preserved(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(0, "SoundExists", b.inline(1))
        b.field(0, "SoundExists", b.inline(1))
        with TempFile(b.build()) as path:
            g = gff.Gff(path)
            self.assertEqual(gff.check(g), [])
            self.assertEqual(g.as_dict(0)["SoundExists"], 1)
            self.assertEqual(len(g.as_pairs(0)), 2)

    def test_single_field_struct_is_direct_row(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.struct(0x64, [2])
        b.field(0, "A", b.inline(1))
        b.field(0, "B", b.inline(2))
        b.field(0, "Only", b.inline(3))
        with TempFile(b.build()) as path:
            g = gff.Gff(path)
            self.assertTrue(g.structs[1].single)
            self.assertEqual(g.structs[1].rows, [2])
            self.assertEqual(gff.check(g), [])


class BoundsTests(unittest.TestCase):
    def assert_clean_error(self, blob, needle):
        with TempFile(blob) as path:
            g = gff.Gff(path)
            errors = gff.check_values(g)
            self.assertTrue(errors, "expected a value error")
            self.assertTrue(any(needle in msg for _, msg in errors), errors)

    def test_string_length_overrun(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        off = len(b.fdata)
        b.fdata += struct.pack("<I", 100) + b"ab"
        b.field(10, "Test", off)
        b.field(0, "Dummy", b.inline(1))
        self.assert_clean_error(b.build(), "string bytes: needs 100 bytes")

    def test_binary_length_overrun(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        off = len(b.fdata)
        b.fdata += struct.pack("<I", 100) + b"ab"
        b.field(13, "Test", off)
        b.field(0, "Dummy", b.inline(1))
        self.assert_clean_error(b.build(), "binary bytes: needs 100 bytes")

    def test_resref_length_overrun(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        off = len(b.fdata)
        b.fdata += bytes([200]) + b"ab"
        b.field(11, "Test", off)
        b.field(0, "Dummy", b.inline(1))
        self.assert_clean_error(b.build(), "resref bytes: needs 200 bytes")

    def test_locstring_count_overrun(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        off = len(b.fdata)
        b.fdata += struct.pack("<3I", 8, 0, 10)
        b.field(12, "Test", off)
        b.field(0, "Dummy", b.inline(1))
        self.assert_clean_error(b.build(), "locstring count 10")

    def test_locstring_total_mismatch(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(12, "Test", b.locstring(0, [(0, "Hello")], total=999))
        b.field(0, "Dummy", b.inline(1))
        self.assert_clean_error(b.build(), "locstring total 999")

    def test_uint64_offset_overrun(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(6, "Test", 1000)
        b.field(0, "Dummy", b.inline(1))
        self.assert_clean_error(b.build(), "uint64: needs 8 bytes")

    def test_struct_field_gap_rejected(self):
        b = Build()
        b.struct(0xFFFFFFFF, [0, 1])
        b.field(0, "A", b.inline(1))
        b.field(0, "B", b.inline(2))
        b.struct_field_gap = 4
        with TempFile(b.build()) as path:
            g = gff.Gff(path)
            self.assertIn("struct table does not end at field table start",
                          gff.check(g))


if __name__ == "__main__":
    unittest.main()
