#!/usr/bin/env python3
"""Synthetic tests for the independent NCS VM."""
from __future__ import annotations

import os
import struct
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, ".."))
sys.path.insert(0, os.path.join(_HERE, "..", ".."))

from vm import ncs_vm  # noqa: E402
from scripts import ncs  # noqa: E402


def _script(body: bytes) -> bytes:
    return b"NCS V1.0" + bytes([0x42]) + struct.pack(">I", 13 + len(body)) + body


def ci(v: int) -> bytes:
    return bytes([0x04, 0x03]) + struct.pack(">i", v)


def cf(v: float) -> bytes:
    return bytes([0x04, 0x04]) + struct.pack(">f", v)


def cs(v: str) -> bytes:
    b = v.encode("latin-1")
    return bytes([0x04, 0x05]) + struct.pack(">H", len(b)) + b


def act(aid: int, argc: int) -> bytes:
    return bytes([0x05, 0x00]) + struct.pack(">H", aid) + bytes([argc])


def jsr(delta: int) -> bytes:
    return bytes([0x1E, 0x00]) + struct.pack(">i", delta)


def jmp(delta: int) -> bytes:
    return bytes([0x1D, 0x00]) + struct.pack(">i", delta)


def movsp(off: int) -> bytes:
    return bytes([0x1B, 0x00]) + struct.pack(">i", off)


def cptop(off: int, size: int) -> bytes:
    return bytes([0x03, 0x01]) + struct.pack(">i", off) + struct.pack(">H", size)


def cpdown(off: int, size: int) -> bytes:
    return bytes([0x01, 0x01]) + struct.pack(">i", off) + struct.pack(">H", size)


def rsadd(q: int) -> bytes:
    return bytes([0x02, q])


def savebp() -> bytes:
    return bytes([0x2A, 0x00])


def restorebp() -> bytes:
    return bytes([0x2B, 0x00])


def cptopbp(off: int, size: int) -> bytes:
    return bytes([0x27, 0x01]) + struct.pack(">i", off) + struct.pack(">H", size)


def cpdownbp(off: int, size: int) -> bytes:
    return bytes([0x26, 0x01]) + struct.pack(">i", off) + struct.pack(">H", size)


def destruct(size: int, off: int, keep: int) -> bytes:
    return bytes([0x21, 0x00]) + struct.pack(">HHH", size, off, keep)


def retn() -> bytes:
    return bytes([0x20, 0x00])


def add(q: int = 0x20) -> bytes:
    return bytes([0x14, q])


def sub(q: int = 0x20) -> bytes:
    return bytes([0x15, q])


def eq(q: int = 0x20) -> bytes:
    return bytes([0x0B, q])


SPECS = {
    "count": 5,
    "actions": [
        {"id": 0, "name": "Random", "return": "int",
         "params": [{"type": "int"}]},
        {"id": 1, "name": "PrintString", "return": "void",
         "params": [{"type": "string"}]},
        {"id": 2, "name": "GetVector", "return": "vector", "params": []},
        {"id": 3, "name": "DelayCommand", "return": "void",
         "params": [{"type": "float"}, {"type": "action"}]},
        {"id": 202, "name": "ActionWait", "return": "void",
         "params": [{"type": "float"}]},
    ],
}


def run(body: bytes, budget: int = 1000) -> dict:
    return ncs_vm.VM(ncs.decode(_script(body)), SPECS, budget=budget).run()


class VMTest(unittest.TestCase):
    def test_const_add_return(self):
        r = run(ci(1) + ci(2) + add() + retn())
        self.assertEqual(r["status"], "returned")
        self.assertEqual(r["top"], {"type": "int", "value": 3})

    def test_string_equality_is_case_insensitive(self):
        r = run(cs("carth") + cs("Carth") + eq(0x23) + retn())
        self.assertEqual(r["status"], "returned")
        self.assertEqual(r["top"], {"type": "int", "value": 1})

    def test_jsr_retn(self):
        # 0: JSR +8 -> 8; 6: RETN; 8: CONST 42; 14: RETN
        r = run(jsr(8) + retn() + ci(42) + retn())
        self.assertEqual(r["status"], "returned")
        self.assertEqual(r["top"], {"type": "int", "value": 42})

    def test_copy_down_copy_up_movsp(self):
        # [7]; CPTOPSP -4 -> [7,7]; CONST 1; ADD -> [7,8];
        # CPDOWNSP -8 writes index 0 -> [8,8]; MOVSP -4 -> [8]
        body = ci(7) + cptop(-4, 4) + ci(1) + add() + cpdown(-8, 4) + movsp(-4) + retn()
        r = run(body)
        self.assertEqual(r["status"], "returned")
        self.assertEqual(r["top"], {"type": "int", "value": 8})

    def test_savebp_restorebp(self):
        # global = 5; SAVEBP; load global; +1; store; clean; restore
        body = (ci(5) + savebp() + cptopbp(-4, 4) + ci(1) + add()
                + cpdownbp(-4, 4) + movsp(-4) + restorebp() + retn())
        r = run(body)
        self.assertEqual(r["status"], "returned")
        self.assertEqual(r["top"], {"type": "int", "value": 6})

    def test_action_void(self):
        r = run(cs("hi") + act(1, 1) + retn())
        self.assertEqual(r["status"], "returned")
        self.assertEqual(r["action_calls"], 1)
        self.assertIsNone(r["top"])

    def test_action_int_stub(self):
        r = run(ci(10) + act(0, 1) + retn())
        self.assertEqual(r["status"], "returned")
        self.assertEqual(r["top"], {"type": "int", "value": 0})

    def test_action_vector_stub(self):
        r = run(act(2, 0) + retn())
        self.assertEqual(r["status"], "returned")
        self.assertEqual(r["max_sp"], 3)
        self.assertEqual(r["top"], {"type": "float", "value": 0.0})

    def test_action_functor_argument_is_not_a_cell(self):
        # DelayCommand(1.0, action): only the float is a stack cell.
        r = run(cf(1.0) + act(3, 2) + retn())
        self.assertEqual(r["status"], "returned")
        self.assertEqual(r["top"], None)

    def test_blocked_action(self):
        r = run(cf(1.0) + act(202, 1) + retn())
        self.assertEqual(r["status"], "blocked")
        self.assertEqual(r["blocked_action"], 202)

    def test_unknown_action(self):
        r = run(act(999, 0) + retn())
        self.assertEqual(r["status"], "error")
        self.assertIn("unknown action", r["error"])

    def test_jump_skips_dead_instruction(self):
        # 0: JMP +12 -> 12; 6: CONST 99; 12: CONST 5; 18: RETN
        r = run(jmp(12) + ci(99) + ci(5) + retn())
        self.assertEqual(r["status"], "returned")
        self.assertEqual(r["top"], {"type": "int", "value": 5})

    def test_recursion_limit(self):
        r = run(jsr(0) + retn())
        self.assertEqual(r["status"], "error")
        self.assertIn("recursion limit", r["error"])

    def test_budget_stop(self):
        r = run(jmp(0), budget=10)
        self.assertEqual(r["status"], "budget")
        self.assertEqual(r["steps"], 10)

    def test_destruct_keeps_selected_slots(self):
        # [1,2,3]; DESTRUCT size=12 (3), offset=4 (1), keep=4 (1) -> [2]
        r = run(ci(1) + ci(2) + ci(3) + destruct(12, 4, 4) + retn())
        self.assertEqual(r["status"], "returned")
        self.assertEqual(r["top"], {"type": "int", "value": 2})

    def test_movsp_pops(self):
        r = run(ci(1) + ci(2) + movsp(-4) + retn())
        self.assertEqual(r["status"], "returned")
        self.assertEqual(r["top"], {"type": "int", "value": 1})


if __name__ == "__main__":
    unittest.main()
