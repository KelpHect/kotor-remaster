#!/usr/bin/env python3
"""State and functional-action tests for the independent NCS runtime."""
from __future__ import annotations

import os
import struct
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, ".."))
sys.path.insert(0, os.path.join(_HERE, "..", ".."))

from vm import ncs_vm  # noqa: E402
from vm.actions_runtime import ActionRuntime  # noqa: E402
from vm.state import World  # noqa: E402
from scripts import ncs  # noqa: E402

V = ncs_vm.Value

SPECS = {"actions": [
    {"id": 580, "name": "GetGlobalNumber", "return": "int",
     "params": [{"type": "string"}]},
    {"id": 581, "name": "SetGlobalNumber", "return": "void",
     "params": [{"type": "string"}, {"type": "int"}]},
    {"id": 681, "name": "GetLocalNumber", "return": "int",
     "params": [{"type": "object"}, {"type": "int"}]},
    {"id": 682, "name": "SetLocalNumber", "return": "void",
     "params": [{"type": "object"}, {"type": "int"}, {"type": "int"}]},
    {"id": 168, "name": "GetTag", "return": "string",
     "params": [{"type": "object"}]},
    {"id": 200, "name": "GetObjectByTag", "return": "object",
     "params": [{"type": "string"}, {"type": "int"}]},
    {"id": 42, "name": "GetIsObjectValid", "return": "int",
     "params": [{"type": "object"}]},
    {"id": 340, "name": "GetNextItemInInventory", "return": "object",
     "params": [{"type": "object"}]},
    {"id": 7, "name": "DelayCommand", "return": "void",
     "params": [{"type": "float"}, {"type": "action"}]},
    {"id": 202, "name": "ActionWait", "return": "void",
     "params": [{"type": "float"}]},
    {"id": 999, "name": "ActionMoveToObject", "return": "void",
     "params": [{"type": "object"}]},
    {"id": 59, "name": "GetStringLength", "return": "int",
     "params": [{"type": "string"}]},
    {"id": 63, "name": "GetStringLeft", "return": "string",
     "params": [{"type": "string"}, {"type": "int"}]},
    {"id": 65, "name": "GetSubString", "return": "string",
     "params": [{"type": "string"}, {"type": "int"}, {"type": "int"}]},
    {"id": 66, "name": "FindSubString", "return": "int",
     "params": [{"type": "string"}, {"type": "string"}]},
]}


def _script(body: bytes) -> bytes:
    return b"NCS V1.0" + bytes([0x42]) + struct.pack(">I", 13 + len(body)) + body


def ci(v: int) -> bytes:
    return bytes([0x04, 0x03]) + struct.pack(">i", v)


def cs(v: str) -> bytes:
    b = v.encode("latin-1")
    return bytes([0x04, 0x05]) + struct.pack(">H", len(b)) + b


def act(aid: int, argc: int) -> bytes:
    return bytes([0x05, 0x00]) + struct.pack(">H", aid) + bytes([argc])


def retn() -> bytes:
    return bytes([0x20, 0x00])


class ActionRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.rt = ActionRuntime(SPECS)
        self.vm = ncs_vm.VM([], SPECS, runtime=self.rt)

    def test_global_set_get(self):
        self.rt.call(581, [[V("string", "g")], [V("int", 42)]], self.vm)
        got = self.rt.call(580, [[V("string", "g")]], self.vm)
        self.assertEqual(got[0].value, 42)
        self.assertEqual(self.rt.world.globals["g"].value, 42)

    def test_local_set_get_by_numeric_index(self):
        oid = self.rt.world.pc.id
        self.rt.call(682, [[V("object", oid)], [V("int", 3)], [V("int", 9)]], self.vm)
        got = self.rt.call(681, [[V("object", oid)], [V("int", 3)]], self.vm)
        self.assertEqual(got[0].value, 9)

    def test_tag_and_validity(self):
        obj = self.rt.world.add_object("trask", area=100)
        got = self.rt.call(200, [[V("string", "trask")], [V("int", 0)]], self.vm)
        self.assertEqual(got[0].value, obj.id)
        tag = self.rt.call(168, [[V("object", obj.id)]], self.vm)
        self.assertEqual(tag[0].value, "trask")
        valid = self.rt.call(42, [[V("object", obj.id)]], self.vm)
        self.assertEqual(valid[0].value, 1)
        missing = self.rt.call(42, [[V("object", 0x7F000000)]], self.vm)
        self.assertEqual(missing[0].value, 0)

    def test_inventory_iterator(self):
        item = self.rt.world.add_object("item", area=100)
        self.rt.world.pc.inventory.append(item.id)
        got = self.rt.call(340, [[V("object", self.rt.world.pc.id)]], self.vm)
        self.assertEqual(got[0].value, item.id)
        end = self.rt.call(340, [[V("object", self.rt.world.pc.id)]], self.vm)
        self.assertEqual(end[0].value, 0x7F000000)

    def test_delay_command_schedules_timer(self):
        self.rt.call(7, [[V("float", 1.5)], []], self.vm)
        self.assertEqual(len(self.rt.world.timers), 1)
        self.assertEqual(self.rt.timers_scheduled, 1)

    def test_action_wait_blocks(self):
        self.rt.call(202, [[V("float", 1.0)]], self.vm)
        self.assertEqual(self.vm.status, "blocked")
        self.assertEqual(self.vm.blocked_action, 202)

    def test_action_queue(self):
        self.rt.call(999, [[V("object", self.rt.world.pc.id)]], self.vm)
        self.assertEqual(len(self.rt.world.action_queue), 1)
        self.assertEqual(self.rt.actions_queued, 1)

    def test_strings(self):
        self.assertEqual(self.rt.call(59, [[V("string", "abcd")]], self.vm)[0].value, 4)
        self.assertEqual(self.rt.call(63, [[V("string", "abcd")], [V("int", 2)]], self.vm)[0].value, "ab")
        self.assertEqual(self.rt.call(65, [[V("string", "abcd")], [V("int", 1)], [V("int", 2)]], self.vm)[0].value, "bc")
        self.assertEqual(self.rt.call(66, [[V("string", "abcd")], [V("string", "cd")]], self.vm)[0].value, 2)

    def test_golden_ncs_trace(self):
        # Arguments are pushed right-to-left: 42, then "g" (param 0 on top).
        body = ci(42) + cs("g") + act(581, 2) + cs("g") + act(580, 1) + retn()
        result = ncs_vm.VM(ncs.decode(_script(body)), SPECS,
                           runtime=self.rt).run()
        self.assertEqual(result["status"], "returned")
        self.assertEqual(result["top"], {"type": "int", "value": 42})
        self.assertEqual(self.rt.world.globals["g"].value, 42)
        self.assertEqual(result["runtime"]["specific"],
                         ["GetGlobalNumber", "SetGlobalNumber"])


class SwmgReturnTest(unittest.TestCase):
    """Regression: typed returns must match the declared action signature."""

    SPECS = {"actions": [
        {"id": 590, "name": "SWMG_AdjustFollowerHitPoints", "return": "int",
         "params": [{"type": "object"}, {"type": "int"}, {"type": "int"}]},
        {"id": 649, "name": "SWMG_SetPlayerSpeed", "return": "void",
         "params": [{"type": "float"}]},
    ]}

    def test_adjust_returns_int_and_set_returns_void(self):
        rt = ActionRuntime(self.SPECS)
        vm = ncs_vm.VM([], self.SPECS, runtime=rt)
        adjust = rt.call(590, [[V("object", rt.world.pc.id)], [V("int", 5)],
                               [V("int", 1)]], vm)
        self.assertEqual(len(adjust), 1)
        self.assertEqual(adjust[0].typ, "int")
        set_speed = rt.call(649, [[V("float", 3.0)]], vm)
        self.assertEqual(set_speed, [])


if __name__ == "__main__":
    unittest.main()
