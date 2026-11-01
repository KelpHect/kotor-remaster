#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/bwm.py. No game data."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bwm


def build_bwm(vc=2, faces=((0, 1, 0),), btype=1, normals=True, mat=True,
              adj=0, aabb=0, edges=0, peri=0, size_extra=0) -> bytes:
    """Build a minimally valid chained BWM."""
    vo = bwm.HEADER_SIZE
    fo = vo + vc * 12
    mo = fo + len(faces) * 12
    no = mo + (len(faces) * 4 if mat else 0)
    do = no + (len(faces) * 12 if (btype == 1 and normals) else 0)
    ao = do
    adjoff = ao + adj * 44
    eo = adjoff + aabb * 12
    po = eo + edges * 8
    end = po + peri * 4 + size_extra
    hdr = bytearray(bwm.HEADER_SIZE)
    hdr[0:8] = bwm.MAGIC
    struct.pack_into("<I", hdr, 8, btype)
    # 5 float triples at 0x0C; position triple (index 4) at 0x3C
    struct.pack_into("<3f", hdr, 0x3C, 1.5, -2.5, 0.25)
    fields = [vc, vo, len(faces), fo,
              mo if mat else 0,
              no if (btype == 1 and normals) else 0,
              do,
              adj, ao, 0, aabb, adjoff, edges, eo, peri, po]
    struct.pack_into("<16I", hdr, 0x48, *fields)
    body = bytearray(end - bwm.HEADER_SIZE)
    for i in range(vc):
        struct.pack_into("<3f", body, vo - bwm.HEADER_SIZE + i * 12,
                         float(i), 0.0, 0.0)
    for i, f in enumerate(faces):
        struct.pack_into("<3I", body, fo - bwm.HEADER_SIZE + i * 12, *f)
    if mat:
        for i in range(len(faces)):
            struct.pack_into("<I", body, mo - bwm.HEADER_SIZE + i * 4, 7)
    if btype == 1 and normals:
        for i in range(len(faces)):
            struct.pack_into("<3f", body, no - bwm.HEADER_SIZE + i * 12, 0, 0, 1)
    return bytes(hdr) + bytes(body)


class TestBwm(unittest.TestCase):
    def test_valid_type1(self):
        b = bwm.parse_bwm(build_bwm())
        self.assertEqual(b.type, 1)
        self.assertEqual(b.vert_count, 2)
        self.assertEqual(b.face_count, 1)
        self.assertEqual(b.position, (1.5, -2.5, 0.25))
        self.assertEqual(b.verts[1], (1.0, 0.0, 0.0))
        self.assertEqual(b.faces[0], (0, 1, 0))
        self.assertEqual(b.face_mats, (7,))
        self.assertEqual(len(b.normals), 1)

    def test_valid_type0_no_normals(self):
        b = bwm.parse_bwm(build_bwm(btype=0, normals=False), expect_type=0)
        self.assertEqual(b.normals, ())

    def test_bad_magic(self):
        with self.assertRaises(bwm.BwmError):
            bwm.parse_bwm(b"BWM V0.9" + bytes(200))

    def test_short(self):
        with self.assertRaises(bwm.BwmError):
            bwm.parse_bwm(b"BWM V1.0")

    def test_face_index_out_of_range(self):
        with self.assertRaises(bwm.BwmError):
            bwm.parse_bwm(build_bwm(faces=((0, 5, 1),)))

    def test_expect_type_mismatch(self):
        with self.assertRaises(bwm.BwmError):
            bwm.parse_bwm(build_bwm(btype=0, normals=False), expect_type=1)

    def test_section_overrun(self):
        data = bytearray(build_bwm(size_extra=0))
        # shrink file so perimeter section (po == len) no longer ends at EOF
        with self.assertRaises(bwm.BwmError):
            bwm.parse_bwm(bytes(data[:-4]))

    def test_chaining_broken(self):
        data = bytearray(build_bwm())
        struct.pack_into("<I", data, 0x48 + 4, bwm.HEADER_SIZE + 4)  # wrong vo
        with self.assertRaises(bwm.BwmError):
            bwm.parse_bwm(bytes(data))

    def test_adjacency_sections(self):
        b = bwm.parse_bwm(build_bwm(adj=2, aabb=1, edges=3, peri=1))
        self.assertEqual(b.fields["adj_count"], 2)
        self.assertEqual(b.fields["peri_count"], 1)


if __name__ == "__main__":
    unittest.main()
