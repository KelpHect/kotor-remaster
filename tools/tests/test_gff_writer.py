from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
import support  # noqa: F401  (sets sys.path and paths)
import gff
import kotor_archive as ka
import kotor_gff_write as gw


def build_tree() -> gw.GffStruct:
    root = gw.GffStruct(0xFFFFFFFF)
    root.add("Tag", "string", "fixture")
    root.add("U8", "uint8", 0xFE)
    root.add("I8", "int8", -3)
    root.add("U16", "uint16", 0xBEEF)
    root.add("I16", "int16", -1234)
    root.add("U32", "uint32", 0xDEADBEEF)
    root.add("I32", "int32", -7)
    root.add("U64", "uint64", 2**40 + 7)
    root.add("I64", "int64", -2**40 - 7)
    root.add("F32", "float", 1.25)
    root.add("F64", "double", -3.5)
    root.add("Str", "string", "hello")
    root.add("Ref", "resref", "abc123")
    root.add("Loc", "locstring", {"strref": 0xFFFFFFFF,
                                 "substrings": [(0, "primary"), (1, "secondary")]})
    root.add("Bin", "binary", b"\x00\x01\xfe\xff")
    root.add("V3", "vector3", (1.0, 2.0, 3.0))
    root.add("V4", "vector4", (4.0, 5.0, 6.0, 7.0))
    child = gw.GffStruct(0x12345678)
    child.add("Name", "string", "child")
    root.add("Child", "struct", child)
    items = []
    for i in range(3):
        item = gw.GffStruct(7)
        item.add("Idx", "int32", i)
        nested = gw.GffStruct(9)
        nested.add("Q", "uint16", i * 10)
        item.add("Nested", "struct", nested)
        items.append(item)
    root.add("Items", "list", items)
    root.add("Empty", "struct", gw.GffStruct(3))
    single = gw.GffStruct(4)
    single.add("Solo", "string", "one")
    root.add("Single", "struct", single)
    return root


class GffWriterTests(unittest.TestCase):
    def test_all_types_roundtrip(self):
        support.ensure_fixtures()
        tmp = support.clean_tmp("gff-all")
        path = tmp / "all.gff"
        original = build_tree()
        gw.write_gff(path, "TEST", original)
        parsed = gff.Gff(str(path))
        self.assertEqual(gff.check(parsed), [])
        # Re-serialize from the parsed tree and compare semantics.
        again = tmp / "again.gff"
        gw.write_gff(again, parsed.magic, gw.from_gff(parsed), parsed.version)
        parsed2 = gff.Gff(str(again))
        self.assertEqual(gff.check(parsed2), [])
        self.assertEqual(gw.semantic(parsed), gw.semantic(parsed2))
        self.assertEqual(parsed.as_dict(0)["Tag"], "fixture")
        self.assertEqual(parsed.as_dict(0)["I8"], -3)
        self.assertEqual(parsed.as_dict(0)["U64"], 2**40 + 7)
        self.assertAlmostEqual(parsed.as_dict(0)["F64"], -3.5)
        self.assertEqual(parsed.as_dict(0)["Loc"]["substrings"][1][1], "secondary")

    def test_limits(self):
        root = gw.GffStruct(0)
        root.add("BadLabelThatIsWayTooLong", "string", "x")
        with self.assertRaises(gw.GffWriteError):
            gw.gff_bytes(b"TEST", root)
        root = gw.GffStruct(0)
        root.add("R", "resref", "12345678901234567")
        with self.assertRaises(gw.GffWriteError):
            gw.gff_bytes(b"TEST", root)

    def test_real_module_gff_semantic_roundtrip(self):
        rim = support.REAL_GAME / "modules" / "M12ab.rim"
        if not rim.exists():
            self.skipTest("reference install not present")
        archive = ka.read_rim(rim)
        with tempfile.TemporaryDirectory(dir=str(support.TMP)) as td:
            for entry in archive.entries:
                if entry.res_type not in (2012, 2014, 2023):
                    continue
                src = Path(td) / f"{entry.res_type}.gff"
                dst = Path(td) / f"{entry.res_type}_rt.gff"
                src.write_bytes(archive.read_entry(entry))
                before = gff.Gff(str(src))
                self.assertEqual(gff.check(before), [])
                gw.write_gff(dst, before.magic, gw.from_gff(before), before.version)
                after = gff.Gff(str(dst))
                self.assertEqual(gff.check(after), [])
                self.assertEqual(gw.semantic(before), gw.semantic(after),
                                 msg=f"real {entry.res_type} semantic mismatch")


if __name__ == "__main__":
    unittest.main()
