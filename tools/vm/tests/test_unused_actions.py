#!/usr/bin/env python3
"""Tests for the 306 corpus-unused action conformance ledger."""
from __future__ import annotations

import json
import os
import sys
import unittest
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent))
sys.path.insert(0, str(_HERE.parent.parent))

from vm import unused_actions  # noqa: E402

REPO = _HERE.parents[2]
SPECS = REPO / "artifacts" / "lanes" / "s2-01-engine-map" / "action_specs.json"
USAGE = REPO / "artifacts" / "lanes" / "s1-04-scripts" / "action_usage_report.json"


class UnusedActionTests(unittest.TestCase):
    def test_306_unused_actions_dispatch(self):
        if not SPECS.exists() or not USAGE.exists():
            self.skipTest("local action specs/usage artifacts not present")
        specs = json.loads(SPECS.read_text(encoding="utf-8"))
        usage = json.loads(USAGE.read_text(encoding="utf-8"))
        report = unused_actions.check(specs, usage["summary"]["actions_unused"])
        self.assertEqual(report["checked"], 306)
        self.assertEqual(report["handlers"].get("default", 0), 0)
        self.assertEqual(report["errors"], [])
        self.assertTrue(all(report["gate"].values()), report["gate"])

    def test_category_groups(self):
        self.assertEqual(unused_actions.category("GetTag"), "query/read")
        self.assertEqual(unused_actions.category("ActionMoveToObject"), "action queue")
        self.assertEqual(unused_actions.category("StringToFloat"), "string")
        self.assertEqual(unused_actions.category("sqrt"), "math")
        self.assertEqual(unused_actions.category("MagicalEffect"), "effect wrapper")


if __name__ == "__main__":
    unittest.main()
