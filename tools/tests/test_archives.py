from __future__ import annotations

import unittest

import support
import kotor_archive as ka


class ArchiveTests(unittest.TestCase):
    def test_erf_roundtrip_duplicates_and_case(self):
        tmp = support.clean_tmp("archives")
        path = tmp / "test.erf"
        entries = [
            ("Alpha", 2017, b"one"),
            ("alpha", 2017, b"two"),      # case variant; first wins
            ("Alpha", 2010, b"ncs"),      # different type; distinct key
            ("Beta", 3007, b"tpc"),
        ]
        ka.write_erf(path, entries, magic=b"MOD ")
        a = ka.read_erf(path)
        self.assertEqual(a.magic, "MOD ")
        self.assertEqual(a.version, "V1.0")
        self.assertEqual(len(a.entries), 4)
        self.assertEqual([e.resref for e in a.entries], ["Alpha", "alpha", "Alpha", "Beta"])
        self.assertEqual(a.read_entry(a.entries[0]), b"one")
        self.assertEqual(a.read_entry(a.entries[2]), b"ncs")
        self.assertEqual(a.lookup("ALPHA", 2017).index, 0)
        self.assertEqual(a.lookup("alpha", 2010).index, 2)
        self.assertIsNone(a.lookup("missing", 2017))
        dups = a.duplicates()
        self.assertEqual(sorted(dups[("alpha", 2017)]), [0, 1])

    def test_rim_roundtrip(self):
        tmp = support.clean_tmp("archives-rim")
        path = tmp / "test.rim"
        entries = [("m01aa", 2012, b"ARE"), ("module", 2014, b"IFO"),
                   ("Alpha", 2010, b"x"), ("alpha", 2010, b"y")]
        ka.write_rim(path, entries)
        a = ka.read_rim(path)
        self.assertEqual(a.format, "RIM")
        self.assertEqual(len(a.entries), 4)
        self.assertEqual(a.read_entry(a.entries[0]), b"ARE")
        self.assertEqual(a.lookup("MODULE", 2014).index, 1)
        self.assertEqual(len(a.duplicates()[("alpha", 2010)]), 2)

    def test_key_bif_roundtrip(self):
        tmp = support.clean_tmp("archives-key")
        ka.write_key_bif(tmp, [
            ("acbonus", 2017, b"2DA"),
            ("Alpha", 2010, b"NCS"),
            ("alpha", 2010, b"NCS2"),
            ("Beta", 3007, b"TPC"),
        ])
        key = ka.read_key(tmp / "chitin.key")
        self.assertEqual(key.bif_count if hasattr(key, "bif_count") else key.header["bif_count"], 1)
        self.assertEqual(key.header["key_count"], 4)
        self.assertEqual(key.stride, 22)
        self.assertEqual(key.bifs[0]["name"].lower(), "data/base.bif")
        hit = key.lookup("ACBONUS", 2017)
        self.assertIsNotNone(hit)
        self.assertEqual(hit.bif_index, 0)
        self.assertEqual(hit.table_index, 0)
        bif = ka.read_bif(tmp / "data" / "base.bif")
        rec = bif.by_id[hit.table_index]
        self.assertEqual(rec.res_type, 2017)
        self.assertEqual(bif.read_record(rec), b"2DA")
        self.assertEqual(len(key.duplicates()[("alpha", 2010)]), 2)

    def test_real_archives(self):
        game = support.REAL_GAME
        if not (game / "chitin.key").exists():
            self.skipTest("reference install not present")
        key = ka.read_key(game / "chitin.key")
        self.assertEqual(key.header["key_count"], 25836)
        self.assertEqual(key.header["bif_count"], 26)
        self.assertEqual(key.stride, 22)
        patch = ka.read_erf(game / "patch.erf")
        self.assertEqual(patch.magic, "ERF ")
        self.assertEqual(len(patch.entries), 97)
        rim = ka.read_rim(game / "modules" / "M12ab.rim")
        self.assertEqual(len(rim.entries), 3)
        self.assertEqual({e.res_type for e in rim.entries}, {2012, 2023, 2014})


if __name__ == "__main__":
    unittest.main()
