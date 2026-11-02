#!/usr/bin/env python3
"""Tests for the Sprint 2 action-spec and engine-map generators."""
from __future__ import annotations

import os
import sys
import unittest

_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)

from analysis import action_specs, engine_map  # noqa: E402


class ActionSpecTests(unittest.TestCase):
    def test_parse_prototypes_and_params(self):
        text = "int Random(int nMaxInteger);\nvoid PrintFloat(float fFloat, int nWidth=18);\n"
        protos = action_specs.parse_prototypes(text)
        self.assertEqual([p["name"] for p in protos], ["Random", "PrintFloat"])
        self.assertEqual(protos[0]["ret"], "int")
        self.assertEqual(protos[1]["params"][1]["optional"], True)
        self.assertEqual(protos[1]["params"][1]["type"], "int")

    def test_stub_text_and_handler_address(self):
        self.assertEqual(action_specs.stub_text("void"), "return")
        self.assertEqual(action_specs.stub_text("vector"),
                         "return vector(0.0, 0.0, 0.0)")
        self.assertEqual(action_specs.stub_text("object"), "return OBJECT_INVALID")
        self.assertEqual(action_specs.handler_address("LAB_005419d0"), "005419d0")


class EngineMapTests(unittest.TestCase):
    def test_g2_closes_with_full_specs_and_handlers(self):
        functions = [
            {"entry": "00401000", "category": "engine", "role": "action_handler"},
            {"entry": "00401060", "category": "engine", "role": ""},
            {"entry": "00401080", "category": "crt_runtime", "role": ""},
        ]
        actions = {
            "counts": {"action_slots": 2},
            "initializer": "0054c960",
            "initializer_size": 8295,
            "dispatch": {"entry": "0052c0d0", "vtable": "0x7450e4"},
        }
        specs = {"actions": [
            {"id": 0, "handler_address": "0x00401000", "handler_present": True,
             "stub": "return 0", "return": "int"},
            {"id": 1, "handler_address": "0x00401060", "handler_present": True,
             "stub": "return", "return": "void"},
        ], "return_types": {"int": 1, "void": 1}}
        m = engine_map.build_map(functions, actions, specs)
        self.assertEqual(m["g2"]["status"], "closed")
        self.assertEqual(m["functions"]["engine"], 2)
        self.assertEqual(m["actions"]["specs"], 2)
        self.assertEqual(m["actions"]["handlers_unique"], 2)
        self.assertEqual(m["actions"]["handlers_present"], 2)
        doc = engine_map.markdown(m)
        self.assertIn("0x7450e4", doc)
        self.assertNotIn("0x0x7450e4", doc)


if __name__ == "__main__":
    unittest.main()
