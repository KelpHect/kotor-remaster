#!/usr/bin/env python3
"""Synthetic tests for the Sprint-1 residual disposition ledger."""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

from scripts import residuals  # noqa: E402


class TestResiduals(unittest.TestCase):
    def _corpus(self):
        return {
            "build_sha256": "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6",
            "decompile_status": {
                "files": 8666,
                "classes": {
                    "exact": {"count": 8428},
                    "compiler-limited": {"count": 228},
                    "header-limited": {"count": 5},
                    "reachable-only": {"count": 5},
                },
            },
            "action_table": {"actions": 772},
        }

    def _usage(self):
        return {"summary": {"actions_unused": list(range(306))}}

    def test_g1_is_closed_and_p3_owns_residuals(self):
        r = residuals.build_residuals(self._corpus(), self._usage())
        self.assertEqual(r["g1"]["status"], "closed")
        self.assertEqual(r["g1"]["open_items"], [])
        self.assertEqual(r["g1"]["classified"], 8666)
        self.assertEqual(r["g1"]["classes"]["compiler-limited"], 228)
        ids = {x["id"]: x for x in r["p3"]}
        self.assertEqual(ids["compiler-limited-recompiles"]["count"], 228)
        self.assertEqual(ids["unused-actions"]["count"], 306)
        self.assertEqual(ids["unused-actions"]["of"], 772)
        closed = {x["id"]: x for x in r["closed_live"]}
        self.assertIn("f4-thumbnail", closed)
        self.assertIn("chargen-play-live", closed)
        self.assertIn("dialogue-live-click", closed)
        self.assertEqual(closed["f4-thumbnail"]["runs"], ["mesa-quicksave7-20261102"])
        self.assertEqual({x["id"] for x in r["later_phase"]}, set())
        static = {x["id"]: x for x in r["closed_static"]}
        self.assertIn("minimap-raster", static)

    def test_missing_status_is_a_g1_item(self):
        corpus = self._corpus()
        corpus["decompile_status"]["files"] = 0
        r = residuals.build_residuals(corpus, self._usage())
        self.assertEqual(r["g1"]["status"], "open")
        self.assertEqual(r["g1"]["open_items"][0]["id"], "ncs-status-missing")

    def test_unclassified_rows_are_a_g1_item(self):
        corpus = self._corpus()
        corpus["decompile_status"]["classes"]["exact"]["count"] = 8000
        r = residuals.build_residuals(corpus, {"summary": {"actions_unused": []}})
        self.assertEqual(r["g1"]["status"], "open")
        self.assertEqual(r["g1"]["open_items"][0]["id"], "ncs-classification-gap")


if __name__ == "__main__":
    unittest.main()
