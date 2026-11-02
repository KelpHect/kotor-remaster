#!/usr/bin/env python3
"""Check that all 772 action specifications dispatch through the independent VM.

For each action, build a synthetic ACTION instruction with the full declared
parameter count, push typed default cells, invoke the VM's mediation hook, and
verify that the typed return-cell count matches the prototype (void 0, vector
3, everything else 1).  No engine code or game data is touched.

Usage:
    python tools/vm/dispatch_check.py --specs <action_specs.json> --out <json>
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, ".."))
sys.path.insert(0, _HERE)

from vm import ncs_vm  # noqa: E402
from scripts import ncs  # noqa: E402

BLOCKING = {202}


def check_specs(specs: dict, use_runtime: bool = False) -> dict:
    errors = []
    runtime = None
    if use_runtime:
        from vm.actions_runtime import ActionRuntime
        runtime = ActionRuntime(specs)
    for a in specs["actions"]:
        aid = a["id"]
        params = a["params"]
        total = len(params)
        vm = ncs_vm.VM([], specs, runtime=runtime)
        # Stack order: last parameter deepest, first parameter on top.
        for k in range(total - 1, -1, -1):
            typ = params[k]["type"]
            if typ == "action":
                vm.pending_functor = True
                continue
            vm.stack.extend(ncs_vm.default_cells(typ))
        blob = bytes([(aid >> 8) & 0xFF, aid & 0xFF, total])
        ins = ncs.Instruction(offset=0, opcode=0x05, name="ACTION", shape="action",
                              qualifier=0, ops=[("c", blob)], raw=blob, size=5)
        try:
            vm._action(ins)
        except Exception as exc:
            errors.append({"id": aid, "name": a["name"], "error": str(exc)})
            continue
        want = 0 if a["return"] == "void" else (3 if a["return"] == "vector" else 1)
        if aid in BLOCKING:
            want = 0
            if vm.status != "blocked":
                errors.append({"id": aid, "name": a["name"],
                               "error": "expected blocked"})
        if len(vm.stack) != want:
            errors.append({"id": aid, "name": a["name"],
                           "error": f"return cells {len(vm.stack)} != {want}"})
    return {"count": len(specs["actions"]), "dispatched": len(specs["actions"]) - len(errors),
            "errors": errors}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--specs", required=True)
    ap.add_argument("--runtime", action="store_true",
                    help="use the functional action runtime")
    ap.add_argument("--out")
    args = ap.parse_args(argv)
    specs = json.loads(Path(args.specs).read_text(encoding="utf-8"))
    result = check_specs(specs, use_runtime=args.runtime)
    text = json.dumps(result, indent=2, sort_keys=True)
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(text + "\n", encoding="utf-8")
    print(json.dumps({k: result[k] for k in ("count", "dispatched", "errors")},
                     sort_keys=True))
    return 0 if not result["errors"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
