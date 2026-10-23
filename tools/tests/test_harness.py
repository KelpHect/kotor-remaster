from __future__ import annotations

import json
import unittest

import support
import build_dynamic_probes as bdp
import vfs_measure as vm


class HarnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        support.ensure_fixtures()
        cls.probes_path = support.TMP / "probes.json"
        cls.probes = bdp.build(support.VFS_GAME, "active",
                               support.VFS_GAME / "saves" / "000002 - TEST",
                               max_per_class=4, max_total=120)
        cls.probes_path.write_text(json.dumps(cls.probes), encoding="utf-8")

    def _matching_events(self) -> list[dict]:
        events = []
        for probe in self.probes["probes"]:
            key = probe["key"]
            if probe["kind"] == "texture_quality":
                pack = probe["packs"][0]
                events.append({"event": "GetResource", "resref": key["resref"],
                               "type": key["type"], "found": True,
                               "table": pack["pack"], "offset": pack["offset"],
                               "size": pack["size"]})
                continue
            exp = probe["expected"]
            events.append({"event": "GetResource", "resref": key["resref"],
                           "type": key["type"], "found": True,
                           "pass": exp["pass"], "table": exp["table"],
                           "offset": exp.get("offset"), "size": exp.get("size")})
        return events

    def test_probe_manifest_shape(self):
        self.assertEqual(self.probes["schema"], "kotor-vfs-probes/1")
        self.assertGreaterEqual(len(self.probes["probes"]), 10)
        self.assertGreaterEqual(self.probes["counts"]["texture_quality"], 1)
        kinds = {p["kind"] for p in self.probes["probes"]}
        self.assertIn("tier", kinds)
        self.assertIn("duplicate", kinds)
        for probe in self.probes["probes"]:
            self.assertIn("key", probe)
            self.assertIn("expected", probe)
            self.assertIn("authority", probe)

    def test_compare_expected_log_matches(self):
        events = self._matching_events()
        log_path = support.TMP / "expected-log.jsonl"
        log_path.write_text("\n".join(json.dumps(e) for e in events), encoding="utf-8")
        loaded = vm.load_events(log_path)
        self.assertEqual(len(loaded), len(events))
        report = vm.compare(self.probes, loaded)
        self.assertEqual(report["counts"].get("mismatch", 0), 0)
        self.assertGreaterEqual(report["counts"].get("match", 0), 10)
        self.assertGreaterEqual(report["counts"].get("observed", 0), 1)
        self.assertTrue(report["texture_quality_counts"])
        # Every normal probe that had an event is match/ambiguous, never unobserved.
        normal = [r for r in report["results"] if r["kind"] != "texture_quality"]
        self.assertTrue(all(r["status"] in ("match", "ambiguous") for r in normal))

    def test_compare_detects_wrong_tier(self):
        events = self._matching_events()
        # Corrupt one non-texture event by claiming the wrong pass/table.
        for event in events:
            if event["type"] == 2010 and event.get("size") == 12:
                event["pass"] = 5
                event["table"] = "chitin.key"
                break
        report = vm.compare(self.probes, events)
        self.assertGreaterEqual(report["counts"].get("mismatch", 0), 1)

    def test_alias_log_fields(self):
        # Lane 06 may emit PascalCase / archive_path aliases.
        raw = [{"Event": "GetResource", "ResRef": "tier_probe", "ResType": 3,
                "found": True, "pass_no": 1, "archive_path": "Override",
                "offset": None, "size": 13}]
        norm = [vm.normalize_event(x) for x in raw]
        self.assertEqual(norm[0]["resref"], "tier_probe")
        self.assertEqual(norm[0]["type"], 3)
        self.assertEqual(norm[0]["pass"], 1)
        self.assertIn("override", norm[0]["table"].lower())


if __name__ == "__main__":
    unittest.main()
