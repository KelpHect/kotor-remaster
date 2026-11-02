#!/usr/bin/env python3
"""Tests for the compiler-codegen reconciliation of the 228 residuals."""
from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

_TOOLS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_TOOLS))

from scripts import (body_order, codegen_reconcile, historical_headers,
                     return_cleanup, struct_source, unreachable)  # noqa: E402

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


class ReturnCleanupTests(unittest.TestCase):
    def test_duplicate_last_local_return(self):
        text = "int f_10() {\n\tint v1;\n\treturn v1;\n}\n"
        new = return_cleanup.add_duplicate_return(text, "10")
        self.assertIsNotNone(new)
        self.assertEqual(new.count("return v1;"), 2)


class BodyOrderTests(unittest.TestCase):
    def test_case_fix_for_stock_lexer(self):
        text = "void f(INT n, OBJECT_ID o);"
        fixed = body_order._compiler_header(text)
        self.assertEqual(fixed, "void f(int n, object o);")


class UnreachableTests(unittest.TestCase):
    def test_five_restored_statements(self):
        self.assertEqual(len(unreachable.RESTORATIONS), 5)
        news = [v for k, v in unreachable.RESTORATIONS.items() if "news" in k]
        self.assertEqual(len(news), 4)
        for text in news:
            self.assertIn('SetGlobalNumber("tat_LinesChecked", 0);', text)
        self.assertIn("GetPartyMemberCount()",
                      unreachable.RESTORATIONS[
                          "k_plev_space_hb__a2b5e977f40bbbce"])


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
