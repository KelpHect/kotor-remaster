"""Byte-parity tests for the lane s1-05 save writer (tools/resources/kotor_save_parity.py).

Synthetic fixtures prove the encoder rules; the retail save slot produced by
the windowed 1280x720 acceptance run (docs/recon/16, profile location
``artifacts/lanes/06-dynamic/profile/saves/000000 - QUICKSAVE``) is analyzed
read-only when present.  Every binary claim targets build SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""
from __future__ import annotations

import struct
import unittest
from pathlib import Path

import support
import kotor_archive as ka
import kotor_gff_write as gw
import kotor_save as ks
import kotor_save_parity as kp

BUILD = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6"
RETAIL_SLOT = (support.REPO_ROOT / "artifacts" / "lanes" / "06-dynamic" / "profile"
               / "saves" / "000000 - QUICKSAVE")


def retail_slot() -> Path | None:
    if (RETAIL_SLOT / "SAVEGAME.sav").is_file():
        return RETAIL_SLOT
    return None


class ErfParitySyntheticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        support.ensure_fixtures()
        cls.tmp = support.clean_tmp("save-parity")

    def test_canonical_fixture_roundtrip(self):
        # ka.erf_bytes (align-4) fixture: verbatim + canonical + align-4 repack
        # are byte-identical; align-1 repack differs only in padding gaps.
        entries = [("alpha", 2010, b"A" * 5), ("Beta", 2017, b"B" * 3),
                   ("alpha", 2010, b"DUP_SECOND")]
        data = ka.erf_bytes(entries, magic=b"MOD ", build_year=126, build_day=296)
        lay = kp.erf_layout(data, "synthetic")
        self.assertTrue(lay["fully_accounted"])
        self.assertTrue(lay["header_tail_all_zero"])
        verbatim, vrep = kp.erf_bytes_verbatim(data, "synthetic")
        self.assertTrue(vrep["byte_identical"])
        rep4, r4 = kp.erf_bytes_repack(data, "synthetic", align=4)
        self.assertTrue(r4["byte_identical"])
        rep1, r1 = kp.erf_bytes_repack(data, "synthetic", align=1)
        self.assertFalse(r1["byte_identical"])
        self.assertLess(r1["length_delta"], 0)  # align-1 packs tighter
        # every differing byte lies in a zero padding gap, never in payloads
        for g in lay["gaps"]:
            self.assertTrue(g["all_zero"])

    def test_retail_style_repack_is_byte_identical(self):
        # build with the from-scratch parity writer (align=1, retail header
        # quirks), then re-parse and repack again: must stay byte-identical
        entries = [("AVAILNPC0", 2027, b"\x00" * 8302),
                   ("INVENTORY", 0, b"I" * 10215),   # retail quirk: type 0
                   ("RePuTe", 2038, b"R" * 27),
                   ("tar_m02af", 2057, ka.erf_bytes([("Module", 2014, b"M" * 100)],
                                                    magic=b"MOD "))]
        base = ka.erf_bytes(entries, magic=b"MOD ")
        data, _ = kp.erf_bytes_repack(base, "x", align=1)
        rep, r = kp.erf_bytes_repack(data, "x", align=1)
        self.assertTrue(r["byte_identical"])
        lay = kp.erf_layout(data, "x")
        self.assertTrue(lay["fully_accounted"])
        self.assertTrue(all(k["res_id_is_index"] for k in lay["keys"]))
        self.assertEqual(lay["header"]["offset_to_localized_string"],
                         lay["header"]["offset_to_key_list"])

    def test_duplicate_and_case_keys_preserved(self):
        entries = [("dup_probe", 2017, b"FIRST"), ("DUP_PROBE", 2017, b"SECOND"),
                   ("MiXeD", 3007, b"CASE")]
        data = ka.erf_bytes(entries)
        rep, r = kp.erf_bytes_repack(data, "dups", align=4)
        self.assertTrue(r["byte_identical"])
        a = ka.read_erf_bytes(rep, "dups")
        dups = a.duplicates()
        self.assertEqual(dups, {("dup_probe", 2017): [0, 1]})
        self.assertEqual(a.lookup("dup_probe", 2017).index, 0)  # first wins
        lay = kp.erf_layout(data, "dups")
        self.assertEqual(lay["keys"][1]["resref"], "DUP_PROBE")  # case kept

    def test_localized_block_preserved(self):
        # hand-build an ERF with one localized string, retail-style placement
        entries = [("a", 10, b"hello")]
        n = len(entries)
        loc = struct.pack("<3I", 1, 0, 2) + b"hi"  # fake localized block
        off_loc = 160
        off_keys = off_loc + len(loc)
        off_res = off_keys + n * 24
        data_start = off_res + n * 8
        header = b"ERF V1.0" + struct.pack("<9I", 1, len(loc), n, off_loc,
                                           off_keys, off_res, 126, 296, 0)
        header = header.ljust(160, b"\x00")
        keys = b"a".ljust(16, b"\x00") + struct.pack("<IHH", 0, 10, 0)
        res = struct.pack("<II", data_start, 5)
        data = header + loc + keys + res + b"hello"
        lay = kp.erf_layout(data, "loc")
        self.assertTrue(lay["fully_accounted"])
        self.assertEqual(lay["localized_block_size"], len(loc))
        verbatim, vrep = kp.erf_bytes_verbatim(data, "loc")
        self.assertTrue(vrep["byte_identical"])
        rep, r = kp.erf_bytes_repack(data, "loc", align=1)
        self.assertTrue(r["byte_identical"])


class GffParitySyntheticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        support.ensure_fixtures()
        cls.tmp = support.clean_tmp("save-parity-gff")

    def _nested(self) -> gw.GffStruct:
        root = gw.GffStruct(0xFFFFFFFF)
        root.add("Name", "string", "root")
        child = gw.GffStruct(0)
        child.add("Tag", "resref", "child")
        child.add("Val", "int32", -7)
        root.add("Child", "struct", child)
        item = gw.GffStruct(1)
        item.add("Label", "string", "x" * 3)
        root.add("Items", "list", [item, item.__class__(1)])
        root.add("Blob", "binary", b"\x01\x02\x03")
        root.add("Loc", "locstring", {"strref": 5, "substrings": [(0, "hey")]})
        return root

    def test_verbatim_and_canonical_on_aligned_fixture(self):
        root = self._nested()
        data = gw.gff_bytes(b"IFO ", root)
        verbatim, vrep = kp.gff_bytes_verbatim(data, "synth")
        self.assertTrue(vrep["byte_identical"])
        lay = kp.gff_layout(data, "synth")
        self.assertTrue(lay["fully_accounted"])
        self.assertTrue(lay["sections_contiguous_from_56"])
        canonical = gw.gff_bytes(b"IFO ", gw.from_gff(ks.gff_from_bytes(data, "s")))
        self.assertEqual(canonical, data)

    def test_parity_align1_semantic_and_idempotent(self):
        root = self._nested()
        data = gw.gff_bytes(b"IFO ", root)  # aligned fdata (canonical writer)
        g = ks.gff_from_bytes(data, "synth")
        parity = kp.gff_bytes_parity(g.magic, gw.from_gff(g), g.version, fdata_align=1)
        rep = kp.diff_report(data, parity, "synth")
        self.assertFalse(rep["byte_identical"])
        self.assertLess(rep["length_delta"], 0)  # align-1 drops padding only
        r = kp.analyze_gff_bytes(data, "synth")
        self.assertTrue(r["semantic_equal_after_reencode"])
        self.assertEqual(r["check_errors_orig"], [])
        # struct/field/label/fidx/lidx section sizes are unchanged; only fdata shrank
        lo, lp = r["layout"], kp.gff_layout(parity, "synth2")
        by_kind_o = {s["kind"]: s for s in lo["sections"]}
        by_kind_p = {s["kind"]: s for s in lp["sections"]}
        for kind in ("struct", "field", "label", "field_index", "list_index"):
            a, b = by_kind_o[kind], by_kind_p[kind]
            self.assertEqual(a.get("count", a.get("size")), b.get("count", b.get("size")),
                             msg=kind)
        self.assertLess(by_kind_p["field_data"]["size"], by_kind_o["field_data"]["size"])
        # the align-1 writer is idempotent: re-encoding its own output is
        # byte-identical (this is the retail-faithful fixed point)
        rep2 = kp.diff_report(parity, kp.gff_bytes_parity(
            g.magic, gw.from_gff(ks.gff_from_bytes(parity, "s2")), g.version,
            fdata_align=1), "synth2")
        self.assertTrue(rep2["byte_identical"])


class RetailSaveParityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.slot = retail_slot()
        if cls.slot is None:
            raise unittest.SkipTest("retail 1280x720 quicksave slot not present")

    def test_install_sentinel(self):
        if not (support.REAL_GAME / "swkotor.exe").is_file():
            self.skipTest("reference install not present")
        info = kp.verify_install(support.REAL_GAME)
        self.assertEqual(info["sha256"], BUILD)
        self.assertTrue(info["match"])

    def test_retail_slot_byte_parity(self):
        rep = kp.analyze_save_folder(self.slot)
        s = rep["summary"]
        self.assertEqual(s["erf_total"], 2)
        self.assertEqual(s["erf_verbatim_identical"], s["erf_total"])
        self.assertEqual(s["erf_repack_align1_identical"], s["erf_total"])
        self.assertEqual(s["gff_total"], 6)
        self.assertEqual(s["gff_verbatim_identical"], s["gff_total"])
        self.assertTrue(s["gff_semantic_equal_all"])
        self.assertEqual(s["gff_check_errors_total"], 0)
        # the flat savenfo.res re-encodes byte-identically from scratch
        self.assertGreaterEqual(s["gff_parity_identical"], 1)
        self.assertTrue(rep["files"]["savenfo.res"]["gff"]["parity_align1"]["byte_identical"])
        self.assertEqual(rep["validation"], {"errors": 0, "warnings": 0})
        # every ERF layout fully accounted (no unexplained bytes)
        def walk(files):
            for f in files.values():
                if "erf" in f:
                    self.assertTrue(f["erf"]["layout"]["fully_accounted"])
                    for ne in f.get("nested", []):
                        self.assertTrue(ne["erf"]["layout"]["fully_accounted"])
                        for g in ne["gffs"]:
                            self.assertTrue(g["layout"]["fully_accounted"])
                if "gff" in f:
                    self.assertTrue(f["gff"]["layout"]["fully_accounted"])
        walk(rep["files"])

    def test_retail_slot_verbatim_rewrite_byte_identical(self):
        tmp = support.clean_tmp("save-parity-rewrite")
        dst = tmp / "000000 - QUICKSAVE"
        rep = kp.rewrite_save_parity(self.slot, dst, overwrite=True, mode="verbatim")
        self.assertEqual(rep["summary"]["files_byte_identical"],
                         rep["summary"]["files_total"])
        self.assertEqual(rep["summary"]["nested_byte_identical"], 1)
        self.assertEqual(rep["rewrite_validation"], {"errors": 0, "warnings": 0})
        # byte-compare every file against the source slot
        for name in ("SAVEGAME.sav", "savenfo.res", "PARTYTABLE.res",
                     "GLOBALVARS.res", "Screen.tga"):
            self.assertEqual((self.slot / name).read_bytes(), (dst / name).read_bytes(),
                             msg=name)

    def test_retail_slot_parity_rewrite_classified(self):
        tmp = support.clean_tmp("save-parity-rewrite2")
        dst = tmp / "000000 - QUICKSAVE"
        rep = kp.rewrite_save_parity(self.slot, dst, overwrite=True, mode="parity")
        self.assertEqual(rep["rewrite_validation"]["errors"], 0)
        # Screen.tga (blob copy) and flat savenfo.res are byte-identical
        self.assertTrue(rep["files"]["Screen.tga"]["rewritten_byte_identical"])
        self.assertTrue(rep["files"]["savenfo.res"]["rewritten_byte_identical"])
        # SAVEGAME.sav differs only inside the nested module's GFF payloads:
        # its own header/keys/resource records are re-encoded identically
        sav = rep["files"]["SAVEGAME.sav"]
        self.assertFalse(sav["rewritten_byte_identical"])
        lay = kp.erf_layout((self.slot / "SAVEGAME.sav").read_bytes(), "SAVEGAME.sav")
        d = sav["diff"]
        self.assertIsNotNone(d)
        # first diff lies inside the nested module payload region, not in the
        # outer header/keys/resource tables
        nested = next(r for r in lay["regions"]
                      if r.get("entry") is not None and r["kind"] == "payload"
                      and lay["keys"][r["entry"]]["res_type"] == 2057)
        self.assertGreaterEqual(d["first_diff_offset"], nested["start"])

    def test_writer_refuses_game_install(self):
        if not (support.REAL_GAME / "chitin.key").exists():
            self.skipTest("reference install not present")
        with self.assertRaises(ks.SaveWriteError):
            kp.rewrite_save_parity(self.slot, support.REAL_GAME / "saves" / "000099 - X")


if __name__ == "__main__":
    unittest.main()
