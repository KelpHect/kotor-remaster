#!/usr/bin/env python3
"""Replay live engine ExecuteCode traces in the independent NCS VM.

The live harness (`tools/harness/live_hooks.js`) records, for each
`ExecuteCode` (0x5d2bd0) call, the instruction stream and, at every ACTION
dispatch (0x52c0d0), the engine's current IP/SP/BP, the pre-action stack
cells, and the post-action stack cells (`ret_cells`, `sp_after`).

This tool runs the same instruction stream in `tools/vm/ncs_vm.py` and
compares, action by action:

* action id, IP, SP and BP before dispatch;
* stack depth after dispatch;
* the return cells (type and raw value), so state-dependent branches are
  replayed with the engine's own action results rather than synthetic
  defaults.

The engine program pointer excludes the 13-byte NCS header; it is wrapped
before decoding.  A trace matches only when the complete action sequence and
every compared cell agree.

Usage:
    python tools/vm/live_diff.py \
        --events artifacts/lanes/s1-06-dynamic/runs/<run>/events.jsonl \
        --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
        --out artifacts/lanes/s3-live/live_diff.json
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from collections import defaultdict
from pathlib import Path

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, ".."))
sys.path.insert(0, _HERE)

from vm import ncs_vm  # noqa: E402
from vm.actions_runtime import ActionRuntime  # noqa: E402
from scripts import ncs  # noqa: E402

TYPE_NAMES = {0x03: "int", 0x04: "float", 0x05: "string", 0x06: "object",
              0x10: "effect", 0x11: "event", 0x12: "location", 0x13: "talent"}


def _bits_to_float(bits: int) -> float:
    bits &= 0xFFFFFFFF
    if bits >= 0x80000000:
        bits -= 0x100000000
    return struct.unpack("<f", struct.pack("<i", bits))[0]


def _cell_value(cell: dict, typ: str) -> ncs_vm.Value:
    if typ == "float":
        return ncs_vm.Value("float", _bits_to_float(cell.get("v", 0)))
    if typ == "object":
        return ncs_vm.Value("object", cell.get("v", 0) & 0xFFFFFFFF)
    if typ == "string":
        text = cell.get("s") or cell.get("str")
        if text is None:
            text = f"@0x{cell.get('v', 0) & 0xFFFFFFFF:08x}"
        return ncs_vm.Value("string", text.rstrip("\x00"))
    return ncs_vm.Value("int", cell.get("v", 0))


class TraceVM(ncs_vm.VM):
    """VM that compares each ACTION to the live engine and injects its returns."""

    def __init__(self, *args, engine_actions=None, **kwargs):
        super().__init__(*args, **kwargs)
        self.engine_actions = engine_actions or []
        self.trace: list[dict] = []
        self.divergence: dict | None = None
        self.injected = True
        self._action_index = 0

    def _action(self, ins):
        aid, _argc = ins.action
        idx = self._action_index
        self._action_index += 1
        pre = {"id": aid, "ip": ins.offset - 13, "sp": len(self.stack),
               "bp": self.bp}
        expected = self.engine_actions[idx] if idx < len(self.engine_actions) else None
        if expected is not None and (pre["id"] != expected["id"] or
                                     pre["ip"] != expected["ip"] or
                                     pre["sp"] != expected["sp"] or
                                     pre["bp"] != expected["bp"]):
            self.divergence = {"at": idx, "kind": "pre", "vm": pre,
                               "engine": {k: expected.get(k) for k in
                                          ("id", "ip", "sp", "bp")}}
        # Engine return injection: run the functional runtime first, then
        # replace its typed default with the exact engine return cells.
        super()._action(ins)
        rec = dict(pre)
        rec["sp_after"] = len(self.stack)
        rec["ret_cells"] = []
        if expected is not None and expected.get("ret_cells"):
            spec = self.specs.get(aid, {})
            ret = spec.get("return", "void")
            if ret == "vector":
                width = 3
            elif ret == "void":
                width = 0
            else:
                width = 1
            cells = expected["ret_cells"][-width:] if width else []
            if width and len(self.stack) >= width:
                injected = []
                for k, cell in enumerate(cells):
                    typ = TYPE_NAMES.get(cell.get("t"), ret)
                    value = _cell_value(cell, typ)
                    if typ == "string" and expected.get("ret_str"):
                        value = ncs_vm.Value("string", expected["ret_str"])
                    injected.append(value)
                self.stack[-width:] = injected
            rec["ret_cells"] = [
                {"t": TYPE_NAMES.get(c.get("t"), "?"), "v": c.get("v")}
                for c in cells
            ]
            if expected.get("sp_after") is not None and \
                    expected["sp_after"] != len(self.stack):
                if self.divergence is None:
                    self.divergence = {
                        "at": idx, "kind": "sp_after",
                        "vm": len(self.stack),
                        "engine": expected["sp_after"]}
        self.trace.append(rec)


def load_traces(path: Path) -> list[dict]:
    """Return per-thread nested ExecuteCode traces with bytecode and actions."""
    programs: dict[tuple, str] = {}
    stacks: dict[int, list[dict]] = defaultdict(list)
    traces: list[dict] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            o = json.loads(line)
        except Exception:
            continue
        kind = o.get("f")
        if kind == "vm_exec":
            key = (o.get("prog"), o.get("len"))
            if o.get("program_hex"):
                programs[key] = o["program_hex"]
            active = {
                "prog": o.get("prog"),
                "len": o.get("len"),
                "hex": o.get("program_hex") or programs.get(key),
                "ip0": o.get("ip") or 0,
                "sp0": o.get("sp") or 0,
                "bp0": o.get("bp") or 0,
                "entry_cells": o.get("entry_cells") or [],
                "actions": [],
                "exit": None,
            }
            stacks[o.get("tid", 0)].append(active)
            traces.append(active)
        elif kind == "vm_action":
            stack = stacks.get(o.get("tid", 0), [])
            if not stack:
                continue
            if o.get("ip") is None:
                continue
            stack[-1]["actions"].append({
                "id": o.get("id"), "ip": o.get("ip"), "sp": o.get("sp"),
                "bp": o.get("bp"), "sp_after": o.get("sp_after"),
                "ret": o.get("ret"), "ret_str": o.get("ret_str"),
                "ret_cells": o.get("ret_cells") or [],
            })
        elif kind == "vm_exec_exit":
            stack = stacks.get(o.get("tid", 0), [])
            if stack:
                stack.pop()["exit"] = {"ret": o.get("ret"), "ip": o.get("ip")}
    return traces


def replay(trace: dict, specs: dict) -> dict:
    if not trace.get("hex"):
        return {"status": "no_bytecode", "match": False}
    body = bytes.fromhex(trace["hex"])
    data = b"NCS V1.0" + bytes([0x42]) + struct.pack(">I", 13 + len(body)) + body
    try:
        instructions = ncs.decode(data)
    except Exception as exc:
        return {"status": "decode_error", "match": False, "error": str(exc)}
    vm = TraceVM(instructions, specs, budget=200_000,
                 runtime=ActionRuntime(specs),
                 engine_actions=trace["actions"])
    if trace.get("entry_cells"):
        seeded = []
        for cell in trace["entry_cells"]:
            typ = TYPE_NAMES.get(cell.get("t"), "int")
            seeded.append(_cell_value(cell, typ))
        vm.stack = seeded
        vm.bp = trace.get("bp0", 0)
    if trace.get("ip0"):
        body_off = {ins.offset - 13: idx for idx, ins in enumerate(instructions)}
        idx = body_off.get(trace["ip0"])
        if idx is None:
            return {"status": "bad_entry_ip", "match": False,
                    "error": f"entry IP 0x{trace['ip0']:X} is not an instruction"}
        vm.ip = idx
    result = vm.run()
    engine = trace["actions"]
    matched = (vm.divergence is None and len(engine) == len(vm.trace))
    if not matched and vm.divergence is None:
        vm.divergence = {
            "at": min(len(engine), len(vm.trace)),
            "kind": "action_count",
            "vm": len(vm.trace), "engine": len(engine)}
    return {
        "status": result["status"],
        "match": matched,
        "engine_actions": len(engine),
        "vm_actions": len(vm.trace),
        "engine_trace": [(a["id"], a["ip"], a["sp"], a["bp"]) for a in engine],
        "vm_trace": [(a["id"], a["ip"], a["sp"], a["bp"]) for a in vm.trace],
        "divergence": vm.divergence,
        "error": result.get("error"),
        "entry": {"ip": trace.get("ip0"), "sp": trace.get("sp0"),
                  "bp": trace.get("bp0")},
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--events", required=True)
    ap.add_argument("--specs", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-traces", type=int, default=200)
    args = ap.parse_args(argv)
    specs = json.loads(Path(args.specs).read_text(encoding="utf-8"))
    traces = load_traces(Path(args.events))
    seen = set()
    results = []
    for tr in traces:
        key = (tr["prog"], tr["len"])
        if key in seen or not tr["hex"]:
            continue
        seen.add(key)
        if len(results) >= args.max_traces:
            break
        r = replay(tr, specs)
        r.update({"prog": tr["prog"], "len": tr["len"]})
        results.append(r)
    matched = sum(1 for r in results if r["match"])
    out = {
        "traces": len(traces),
        "unique_programs": len(seen),
        "replayed": len(results),
        "matched": matched,
        "mismatched": len(results) - matched,
        "results": results,
    }
    p = Path(args.out)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(out, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({k: out[k] for k in (
        "traces", "unique_programs", "replayed", "matched", "mismatched")},
        sort_keys=True))
    return 0 if matched == len(results) and results else 1


if __name__ == "__main__":
    raise SystemExit(main())
