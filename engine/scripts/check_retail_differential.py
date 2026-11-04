#!/usr/bin/env python3
"""Retail-vs-engine semantic action differential.

The recorded s4 retail traces are new-game/Endar Spire sessions. This checker
compares their executed action IDs with the engine's deterministic Endar Spire
trace, the engine's functional VM corpus (including drained delayed/queued
closures), and the implemented action table. The comparison is restricted to
actions the shipped module scripts actually use; menu/chargen-only action ids
are reported separately.

    python engine/scripts/check_retail_differential.py \
        --engine-trace docs/evidence/s14/engine-semantic-end_m01aa.jsonl \
        --retail-trace artifacts/lanes/s4-trace/live-05-semantic.jsonl \
        --vm-coverage docs/evidence/s14/vm-actions.jsonl \
        --module-usage artifacts/lanes/s1-04-scripts/module_action_usage.csv \
        --module end_m01aa \
        --action-table engine/src/script/ActionTable.inc \
        --out docs/evidence/s14/retail-differential.json
"""
from __future__ import annotations

import argparse
import collections
import csv
import json
import re
from pathlib import Path


def read_jsonl(path: Path) -> list[dict]:
    rows = []
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def action_sequence(rows: list[dict]) -> list[int]:
    out = []
    for row in rows:
        if row.get("kind") != "kotor.action":
            continue
        action = row.get("payload", {}).get("action")
        if isinstance(action, int):
            out.append(action)
    return out


def trace_details(rows: list[dict]) -> dict:
    phases = collections.Counter()
    choices = 0
    transitions = 0
    for row in rows:
        kind = row.get("kind")
        if kind == "kotor.action":
            phases[row.get("payload", {}).get("phase", "unknown")] += 1
        elif kind == "kotor.dialogue":
            choices += 1
        elif kind == "kotor.transition":
            transitions += 1
    return {"choices": choices, "transitions": transitions,
            "phases": dict(sorted(phases.items()))}


def trace_header(rows: list[dict]) -> dict:
    for row in rows:
        if row.get("kind") == "header":
            return row.get("payload", {})
    return {}


def module_actions(path: Path, module: str) -> dict[int, str]:
    actions: dict[int, str] = {}
    with path.open(encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            if row["module"].lower() != module.lower():
                continue
            actions[int(row["action_id"])] = row["action_name"]
    return actions


def implemented_actions(path: Path) -> set[int]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return {int(value) for value in re.findall(r"ACTION\((\d+),", text)}


def action_names(path: Path) -> dict[int, str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return {int(action): name for action, name in
            re.findall(r'ACTION\((\d+), "([^"]+)"', text)}


def vm_actions(paths: list[Path]) -> set[int]:
    actions: set[int] = set()
    for path in paths:
        for row in read_jsonl(path):
            for key in ("executed_actions",):
                actions.update(int(value) for value in row.get(key, []))
            actions.update(int(value) for value in row.get("actions", {}))
    return actions


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-trace", required=True)
    parser.add_argument("--retail-trace", required=True)
    parser.add_argument("--vm-coverage", action="append", default=[])
    parser.add_argument("--module-usage", required=True)
    parser.add_argument("--module", default="end_m01aa")
    parser.add_argument("--action-table", required=True)
    parser.add_argument("--expect-choices", type=int, default=0)
    parser.add_argument("--expect-transitions", type=int, default=0)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    engine_rows = read_jsonl(Path(args.engine_trace))
    retail_rows = read_jsonl(Path(args.retail_trace))
    engine = action_sequence(engine_rows)
    retail = action_sequence(retail_rows)
    engine_set, retail_set = set(engine), set(retail)
    vm = vm_actions([Path(path) for path in args.vm_coverage])
    used = module_actions(Path(args.module_usage), args.module)
    implemented = implemented_actions(Path(args.action_table))
    names = action_names(Path(args.action_table))

    retail_script = retail_set & set(used)
    executed = engine_set | vm
    missing_execution = sorted(retail_script - executed)
    missing_implementation = sorted(retail_script - implemented)
    missing_all_execution = sorted(retail_set - executed)
    missing_all_implementation = sorted(retail_set - implemented)
    retail_only = sorted(retail_set - set(used))
    engine_only = sorted(engine_set - retail_set)

    def labelled(ids: list[int]) -> list[dict]:
        return [{"action": action, "name": names.get(action, "")} for action in ids]

    details = trace_details(engine_rows)
    engine_header = trace_header(engine_rows)
    retail_header = trace_header(retail_rows)
    engine_module = str(engine_header.get("module", ""))
    retail_modules = [str(value).lower()
                      for value in retail_header.get("modules", [])]
    engine_module_ok = (not engine_module
                        or engine_module.lower() == args.module.lower())
    retail_module_ok = (not retail_modules
                        or args.module.lower() in retail_modules)
    report = {
        "schema": "kotor.retail-differential/1",
        "module": args.module,
        "engine_trace": str(args.engine_trace),
        "retail_trace": str(args.retail_trace),
        "vm_coverage": list(args.vm_coverage),
        "retail_actions": len(retail),
        "retail_unique": len(retail_set),
        "engine_actions": len(engine),
        "engine_unique": len(engine_set),
        "vm_unique": len(vm),
        "module_used_actions": len(used),
        "retail_script_actions": len(retail_script),
        "executed_union": len(executed),
        "missing_execution": labelled(missing_execution),
        "missing_implementation": labelled(missing_implementation),
        "missing_all_execution": missing_all_execution,
        "missing_all_implementation": missing_all_implementation,
        "retail_only_not_in_module": retail_only,
        "engine_only": labelled(engine_only),
        "engine_details": details,
        "engine_module": engine_module,
        "retail_modules": retail_modules,
        "engine_module_ok": engine_module_ok,
        "retail_module_ok": retail_module_ok,
        "expected_choices": args.expect_choices,
        "expected_transitions": args.expect_transitions,
        "choices_ok": args.expect_choices == 0
            or details["choices"] == args.expect_choices,
        "transitions_ok": args.expect_transitions == 0
            or details["transitions"] == args.expect_transitions,
        "pass": not missing_execution and not missing_implementation
            and not missing_all_execution and not missing_all_implementation
            and engine_module_ok and retail_module_ok
            and (args.expect_choices == 0
                 or details["choices"] == args.expect_choices)
            and (args.expect_transitions == 0
                 or details["transitions"] == args.expect_transitions),
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                   encoding="utf-8")
    print(json.dumps({key: report[key] for key in
                      ("module", "retail_script_actions", "executed_union",
                       "missing_execution", "missing_implementation",
                       "missing_all_execution", "missing_all_implementation",
                       "pass")},
                     sort_keys=True))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
