#!/usr/bin/env python3
"""Synthetic tests for the live ExecuteCode trace comparator."""
from __future__ import annotations

import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent))
sys.path.insert(0, str(_HERE.parent.parent))

from vm import live_diff  # noqa: E402

SPECS = {"actions": [
    {"id": 0, "name": "Random", "return": "int",
     "params": [{"type": "int"}]},
]}


def ci(v: int) -> bytes:
    return bytes([0x04, 0x03]) + struct.pack(">i", v)


def act(aid: int, argc: int) -> bytes:
    return bytes([0x05, 0x00]) + struct.pack(">H", aid) + bytes([argc])


def retn() -> bytes:
    return bytes([0x20, 0x00])


class LiveDiffTests(unittest.TestCase):
    def _events(self, ret_value: int, sp_after: int = 1) -> Path:
        body = ci(1) + act(0, 1) + retn()
        events = [
            {"f": "vm_exec", "prog": "0x1000", "len": len(body), "ip": 0,
             "sp": 0, "bp": 0, "tid": 1, "program_hex": body.hex(),
             "entry_cells": []},
            {"f": "vm_action", "id": 0, "ip": 6, "sp": 1, "bp": 0,
             "sp_after": sp_after, "ret": ret_value, "tid": 1,
             "ret_cells": [{"t": 3, "v": ret_value}]},
            {"f": "vm_exec_exit", "ret": 0, "ip": len(body), "tid": 1},
        ]
        tmp = tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False,
                                          encoding="utf-8")
        for row in events:
            tmp.write(json.dumps(row) + "\n")
        tmp.close()
        self.addCleanup(lambda: Path(tmp.name).unlink(missing_ok=True))
        return Path(tmp.name)

    def test_exact_replay_with_engine_return_injection(self):
        trace = live_diff.load_traces(self._events(42))[0]
        result = live_diff.replay(trace, SPECS)
        self.assertTrue(result["match"], result)
        self.assertEqual(result["engine_trace"], [(0, 6, 1, 0)])
        self.assertEqual(result["vm_trace"], [(0, 6, 1, 0)])
        self.assertEqual(result["status"], "returned")

    def test_mismatch_is_reported(self):
        # The engine claims an extra stack cell; the VM must not match.
        trace = live_diff.load_traces(self._events(42, sp_after=2))[0]
        result = live_diff.replay(trace, SPECS)
        self.assertFalse(result["match"])
        self.assertEqual(result["divergence"]["kind"], "sp_after")


if __name__ == "__main__":
    unittest.main()
