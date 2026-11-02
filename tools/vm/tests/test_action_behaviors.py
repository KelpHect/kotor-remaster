#!/usr/bin/env python3
"""Behavioral conformance test for the 306 corpus-unused actions."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent))
sys.path.insert(0, str(_HERE.parent.parent))

from vm import behavior_suite  # noqa: E402

REPO = _HERE.parents[2]
SPECS = REPO / "artifacts" / "lanes" / "s2-01-engine-map" / "action_specs.json"
USAGE = REPO / "artifacts" / "lanes" / "s1-04-scripts" / "action_usage_report.json"


class ActionBehaviorTests(unittest.TestCase):
    def test_306_unused_actions_have_behavior(self):
        if not SPECS.exists() or not USAGE.exists():
            self.skipTest("local action specs/usage artifacts not present")
        specs = json.loads(SPECS.read_text(encoding="utf-8"))
        usage = json.loads(USAGE.read_text(encoding="utf-8"))
        report = behavior_suite.check(specs, usage["summary"]["actions_unused"])
        self.assertEqual(report["checked"], 306)
        self.assertEqual(report["handlers"]["default"], 0)
        self.assertEqual(report["errors"], [])
        self.assertTrue(all(report["gate"].values()), report["gate"])
        self.assertGreaterEqual(report["state_derived_assertions"], 80)


if __name__ == "__main__":
    unittest.main()
