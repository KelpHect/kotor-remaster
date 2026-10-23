from __future__ import annotations

import shutil
import unittest

import support
import kotor_inventory as ki
import kotor_save as ks


def structural(model: dict) -> dict:
    modules = []
    for m in model.get("modules", []):
        ifo = m.get("ifo") or {}
        git = m.get("git") or {}
        are = m.get("are") or {}
        modules.append({
            "resref": m.get("resref"),
            "entry_area": (ifo.get("entry") or {}).get("area"),
            "entry": (ifo.get("entry") or {}),
            "area_list": ifo.get("area_list"),
            "git_use_templates": git.get("use_templates"),
            "git_counts": git.get("object_counts"),
            "git_total": git.get("total_instances"),
            "are_rooms": are.get("rooms"),
            "has_ifo": bool(ifo), "has_are": bool(are), "has_git": bool(git),
        })
    pt = model.get("party_table") or {}
    gv = model.get("global_vars") or {}
    nfo = model.get("nfo") or {}
    inv = model.get("inventory") or {}
    return {
        "nfo": {k: nfo.get(k) for k in
                ("area_name", "last_module", "save_game_name", "pc_name",
                 "time_played", "cheat_used", "portraits", "screenshot")},
        "modules": modules,
        "party": {k: pt.get(k) for k in
                  ("gold", "member_count", "members", "available_npc_count",
                   "journal_count", "journal_states", "journal_sample",
                   "chemical_count", "component_count")},
        "globals": {k: gv.get(k) for k in
                    ("category_counts", "numbers", "booleans", "strings",
                     "locations", "boolean_true_count")},
        "inventory": {k: inv.get(k) for k in ("item_count", "items")},
    }


class SaveParserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        support.ensure_fixtures()

    def test_manual_save_structure(self):
        model = ks.SaveFolder(support.SAVE_FIXTURES / "000002 - SYNTHETIC SAVE").parse()
        self.assertEqual(model["slot"], 2)
        self.assertEqual(model["kind"], "manual")
        self.assertFalse(model["autosave"])
        self.assertEqual(model["outer"]["entry_count"], 5)
        self.assertEqual(len(model["modules"]), 1)
        m = model["modules"][0]
        self.assertEqual(m["resref"], "m99aa")
        self.assertEqual(m["nested_magic"], "MOD ")
        self.assertTrue(m["ifo"] and m["are"] and m["git"])
        self.assertEqual(m["ifo"]["entry"]["area"], "m99aa")
        self.assertAlmostEqual(m["ifo"]["entry"]["x"], 1.5)
        self.assertAlmostEqual(m["ifo"]["entry"]["y"], -2.5)
        self.assertAlmostEqual(m["ifo"]["entry"]["z"], 0.25)
        self.assertEqual(m["ifo"]["area_list"], ["m99aa"])
        self.assertEqual(m["ifo"]["tag"], "MODULE")
        self.assertEqual(m["are"]["tag"], "m99aa")
        self.assertEqual(m["are"]["rooms"], 1)
        self.assertEqual(m["git"]["use_templates"], 0)
        self.assertEqual(m["git"]["object_counts"]["creatures"], 2)
        self.assertEqual(m["git"]["object_counts"]["doors"], 1)
        self.assertEqual(m["git"]["object_counts"]["placeables"], 2)
        self.assertEqual(m["git"]["total_instances"], 5)

        pt = model["party_table"]
        self.assertEqual(pt["gold"], 1234)
        self.assertEqual(pt["member_count"], 2)
        self.assertEqual([x["leader"] for x in pt["members"]], [True, False])
        self.assertEqual([x["member_id"] for x in pt["members"]], [0, 1])
        self.assertEqual(pt["available_npc_count"], 2)
        self.assertEqual(pt["selectable_npc_count"], 1)
        self.assertEqual(pt["journal_count"], 2)
        self.assertEqual(pt["journal_states"], {"1": 1, "2": 1})
        self.assertEqual(pt["chemical_count"], 4)
        self.assertEqual(pt["component_count"], 5)

        gv = model["global_vars"]
        self.assertEqual(gv["category_counts"], {"number": 3, "boolean": 3,
                                                 "string": 2, "location": 1})
        self.assertEqual([row["value"] for row in gv["numbers"]], [42, 7, 255])
        self.assertEqual([row["value"] for row in gv["booleans"]], [True, False, True])
        self.assertEqual([row["value"] for row in gv["strings"]], ["alpha", "beta"])
        self.assertEqual(gv["locations"][0]["value"], [1.0, 2.0, 3.0, 4.0, 5.0, 6.0])
        self.assertEqual(gv["checks"], [])

        nfo = model["nfo"]
        self.assertEqual(nfo["last_module"], "m99aa")
        self.assertEqual(nfo["area_name"], "Synthetic Area")
        self.assertEqual(nfo["time_played"], 123)
        self.assertEqual(nfo["portraits"], ["po_pc", "po_party1", "po_party2"])
        self.assertEqual(nfo["screenshot"], "loadscreen")
        self.assertGreater(nfo["timestamp_unix"], 0)

        self.assertEqual(model["inventory"]["item_count"], 2)
        self.assertEqual(model["repute"]["list_counts"]["FactionList"], 2)
        self.assertEqual(len(model["pcs"]), 1)
        self.assertEqual(len(model["avail_npcs"]), 1)
        self.assertEqual(model["validation"]["errors"], [])
        self.assertEqual(model["validation"]["warnings"], [])

    def test_autosave_rules(self):
        model = ks.SaveFolder(support.SAVE_FIXTURES / "000001 - AUTOSAVE").parse()
        self.assertTrue(model["autosave"])
        self.assertEqual(model["kind"], "autosave")
        self.assertTrue(model["pifo"]["present"])
        self.assertFalse(model["screen"]["present"])
        self.assertEqual(model["validation"]["errors"], [])
        self.assertEqual(model["validation"]["warnings"], [])

    def test_corrupt_sentinel(self):
        model = ks.SaveFolder(support.SAVE_FIXTURES / "000003 - CORRUPT TEST").parse()
        codes = {e["code"] for e in model["validation"]["errors"]}
        self.assertIn("corrupt.sentinel", codes)

    def test_missing_sidecar(self):
        tmp = support.clean_tmp("save-missing")
        shutil.copytree(support.SAVE_FIXTURES / "000002 - SYNTHETIC SAVE", tmp / "save")
        (tmp / "save" / "GLOBALVARS.res").unlink()
        model = ks.SaveFolder(tmp / "save").parse()
        codes = {e["code"] for e in model["validation"]["errors"]}
        self.assertIn("global_vars.missing", codes)

    def test_rewrite_semantic_roundtrip(self):
        tmp = support.clean_tmp("save-rewrite")
        source = support.SAVE_FIXTURES / "000002 - SYNTHETIC SAVE"
        before = ks.SaveFolder(source).parse()
        after = ks.SaveFolder(source).rewrite_to(tmp / "000009 - REWRITE")
        self.assertEqual(structural(before), structural(after))
        self.assertEqual(after["validation"]["errors"], [])
        self.assertEqual(after["validation"]["warnings"], [])
        # Every embedded GFF still passes the project's structural checker.
        for m in after["modules"]:
            for key in ("ifo", "are", "git"):
                self.assertEqual(m[key]["check_errors"], [], msg=key)
        self.assertEqual(before["nfo"]["check_errors"], [])
        self.assertEqual(after["nfo"]["check_errors"], [])

    def test_existing_project_erf_parser_accepts_writer_output(self):
        path = support.SAVE_FIXTURES / "000002 - SYNTHETIC SAVE" / "SAVEGAME.sav"
        parsed = ki.parse_erf(str(path))
        self.assertIsNotNone(parsed)
        self.assertEqual(parsed["layout"], "kotor-v1")
        self.assertEqual(parsed["entry_count"], 5)
        nested_entry = parsed["entries"][0]
        self.assertEqual(nested_entry["resref"], "m99aa")
        raw = path.read_bytes()
        nested = raw[nested_entry["offset"]:nested_entry["offset"] + nested_entry["size"]]
        tmp = support.clean_tmp("save-crossparser")
        (tmp / "nested.sav").write_bytes(nested)
        nested_parsed = ki.parse_erf(str(tmp / "nested.sav"))
        self.assertEqual(nested_parsed["entry_count"], 3)
        self.assertEqual({e["type"] for e in nested_parsed["entries"]}, {2012, 2014, 2023})

    def test_real_save_if_present(self):
        folders = support.real_save_folders()
        if not folders:
            self.skipTest("no real save folder exists in the reference install")
        model = ks.SaveFolder(folders[0]).parse()
        self.assertTrue(model["outer"]["present"])
        self.assertGreaterEqual(len(model["modules"]), 1)


if __name__ == "__main__":
    unittest.main()
