#!/usr/bin/env python3
"""Tests for the compiler-codegen reconciliation of the 228 residuals."""
from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

_TOOLS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_TOOLS))

from scripts import codegen_reconcile, historical_headers, struct_source  # noqa: E402

REPO = _TOOLS.parent
ORIG = REPO / "artifacts" / "lanes" / "04-ncs" / "extracted" / "unique"
NEW = REPO / "artifacts" / "lanes" / "ncs-decompiler" / "cont2" / "out" / "ncs"

SAMPLES = {
    "k33_mek_attackgu__a22245e1d920e70a": "struct-group",
    "k_pebn_loc01__87612f468dfa59bd": "default-args",
    "k_pend_chest02__8dc902eaa3ab1a42": "body-order",
}


class HistoricalHeaderTests(unittest.TestCase):
    def test_trim_prototype_to_historical_arity(self):
        line = ("void ShowPartySelectionGUI(string sExitScript = \"\", "
                "int nForceNPC1 = -1, int nForceNPC2 = -1);")
        trimmed = historical_headers._trim_prototype(
            line, "ShowPartySelectionGUI", 1)
        self.assertEqual(
            trimmed,
            "void ShowPartySelectionGUI(string sExitScript = \"\");")

    def test_split_params_keeps_nested_commas(self):
        params = historical_headers._split_params(
            "object o, vector v = [0.0,0.0,0.0], int n")
        self.assertEqual(len(params), 3)
        self.assertEqual(params[1], "vector v = [0.0,0.0,0.0]")


class StructSourceTests(unittest.TestCase):
    def test_rewrite_scalar_globals_to_struct(self):
        text = ("int g1;\nint g2;\nvoid main() { g2 = 1; }\n")
        new, applied = struct_source._rewrite(
            text, [{"base": -8, "size": 8, "fields": [4]}])
        self.assertEqual(len(applied), 1)
        self.assertIn("struct S8 { int f0; int f1; };", new)
        self.assertIn("struct S8 s8;", new)
        self.assertIn("s8.f1 = 1;", new)
        self.assertNotIn("\nint g1;", new)


class CodegenReconcileTests(unittest.TestCase):
    def test_three_known_transforms(self):
        missing = [n for n in SAMPLES if not (ORIG / f"{n}.ncs").exists()
                   or not (NEW / f"{n}.ncs").exists()]
        if missing:
            self.skipTest("local corpus/recompiler artifacts not present")
        for name, transform in SAMPLES.items():
            with self.subTest(name=name):
                orig = (ORIG / f"{name}.ncs").read_bytes()
                new = (NEW / f"{name}.ncs").read_bytes()
                rec = codegen_reconcile.reconcile(orig, new)
                self.assertTrue(rec["equal"], rec)
                self.assertEqual(rec["transform"], transform)


if __name__ == "__main__":
    unittest.main()
