#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/formats/mdl.py. No game data."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import mdl


def build_mdl(nodes=1, name=b"testmodel", supermodel=b"NULL") -> bytes:
    """Build a minimal MDL with one root node (type 0) and one child (33)."""
    # layout: 12-byte file header, then data area at 12
    # name offset table at data+196 holding 2 offsets (rel to 12)
    name_tbl_off = 196  # data offset
    names = [b"root\x00", b"mesh\x00"]
    name_blob_off = name_tbl_off + 8  # after 2 u32s
    name0 = name_blob_off
    name1 = name_blob_off + len(names[0])
    # node 0 at data+220 (96-byte record), node 1 at data+316
    node0_off = 220
    node1_off = 220 + 96
    total = 12 + node1_off + 80
    buf = bytearray(total)
    struct.pack_into("<I", buf, 0, 0)               # first u32 == 0
    struct.pack_into("<I", buf, 4, total - 20)
    struct.pack_into("<I", buf, 8, 0x1000)
    struct.pack_into("<I", buf, 0x0C, mdl.K1_PC_ID)
    struct.pack_into("<I", buf, 0x10, 0x405520)
    buf[0x14:0x14 + 32] = name.ljust(32, b"\x00")
    struct.pack_into("<2I", buf, 0x34, node0_off, nodes + 1)
    struct.pack_into("<2I", buf, 0x70, 0, 0)         # no animations
    buf[0x94:0x94 + 32] = supermodel.ljust(32, b"\x00")
    struct.pack_into("<6I", buf, 0xB8, 0, 4096, 0, name_tbl_off, 2, 2)
    struct.pack_into("<2I", buf, 12 + name_tbl_off, name0, name1)
    buf[12 + name0:12 + name0 + len(names[0])] = names[0]
    buf[12 + name1:12 + name1 + len(names[1])] = names[1]

    def node(off, ntype, nindex, nname, parent, pos, quat, kids):
        a = 12 + off
        struct.pack_into("<4H", buf, a, ntype, nindex, nname, 0)
        struct.pack_into("<I", buf, a + 12, parent)
        struct.pack_into("<3f", buf, a + 16, *pos)
        struct.pack_into("<4f", buf, a + 28, *quat)
        if kids:
            child_arr = off + 80  # inside this node's own record (96 B)
            struct.pack_into("<3I", buf, a + 44, child_arr, len(kids), len(kids))
            for i, k in enumerate(kids):
                struct.pack_into("<I", buf, 12 + child_arr + i * 4, k)
        else:
            struct.pack_into("<3I", buf, a + 44, 0, 0, 0)
        struct.pack_into("<6I", buf, a + 56, 0, 0, 0, 0, 0, 0)

    node(node1_off, 33, 1, 1, node0_off, (1.0, 2.0, 3.0), (1.0, 0, 0, 0), ())
    node(node0_off, 0, 0, 0, 0, (0.0, 0.0, 0.0), (1.0, 0, 0, 0), (node1_off,))
    return bytes(buf)


class TestMdl(unittest.TestCase):
    def test_valid(self):
        m = mdl.parse_mdl(build_mdl())
        self.assertEqual(m.name, "testmodel")
        self.assertEqual(m.supermodel, "NULL")
        self.assertEqual(len(m.nodes), 2)
        root = m.nodes[0]
        self.assertEqual(root.name, "root")
        self.assertEqual(root.children, (m.nodes[1].offset,))
        mesh = m.nodes[1]
        self.assertEqual(mesh.type, 33)
        self.assertEqual(mesh.pos, (1.0, 2.0, 3.0))
        self.assertEqual(mesh.quat, (1.0, 0.0, 0.0, 0.0))

    def test_bad_first_word(self):
        data = bytearray(build_mdl())
        struct.pack_into("<I", data, 0, 5)
        with self.assertRaises(mdl.MdlError):
            mdl.parse_mdl(bytes(data))

    def test_bad_geom_id(self):
        data = bytearray(build_mdl())
        struct.pack_into("<I", data, 0x0C, 0x12345678)
        with self.assertRaises(mdl.MdlError):
            mdl.parse_mdl(bytes(data))

    def test_node_count_mismatch_is_warning(self):
        # shipped quirk: some models declare more nodes than reachable
        data = bytearray(build_mdl())
        struct.pack_into("<I", data, 0x38, 99)
        m = mdl.parse_mdl(bytes(data))
        self.assertTrue(any("node count" in w for w in m.warnings))

    def test_cycle_detected(self):
        data = bytearray(build_mdl())
        # point root's child array at the root itself
        struct.pack_into("<3I", data, 12 + 220 + 44, 220 + 80, 1, 1)
        struct.pack_into("<I", data, 12 + 220 + 80, 220)
        with self.assertRaises(mdl.MdlError):
            mdl.parse_mdl(bytes(data))

    def test_truncated(self):
        with self.assertRaises(mdl.MdlError):
            mdl.parse_mdl(b"\x00" * 16)


if __name__ == "__main__":
    unittest.main()
