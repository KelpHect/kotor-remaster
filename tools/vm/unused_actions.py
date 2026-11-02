#!/usr/bin/env python3
"""Exercise the 306 actions not used by the shipped corpus.

The shipped corpus uses 466 of the 772 action ids.  The remaining 306 are
reachable only by modded content, but the independent runtime still has to
define them.  This tool builds a synthetic ACTION call for every unused id
with typed default arguments, invokes the functional runtime, and verifies:

* the id resolves to a handler (specific or generic group);
* no call falls through to the unclassified default;
* the return cell count matches the prototype (void 0, vector 3, else 1);
* no exception is raised.

It writes a machine-readable ledger and a tracked Markdown report.

Usage:
    python tools/vm/unused_actions.py \
        --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
        --usage artifacts/lanes/s1-04-scripts/action_usage_report.json \
        --out-json artifacts/lanes/s3-02-actions/unused_actions.json \
        --out-doc docs/recon/30_UNUSED_ACTIONS.md
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter
from pathlib import Path

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, ".."))
sys.path.insert(0, _HERE)

from vm import ncs_vm  # noqa: E402
from vm.actions_runtime import ActionRuntime  # noqa: E402

EXPECTED_UNUSED = 306


def category(name: str) -> str:
    if name.startswith(("Get", "Is", "Has", "Find")):
        return "query/read"
    if name.startswith("Action"):
        return "action queue"
    if name.startswith(("Set", "Clear", "Stop", "Destroy", "Remove", "Apply",
                        "Change", "Face", "Jump", "Move", "Delay", "Assign",
                        "Play", "Sound", "Music", "Speak", "Effect", "Print",
                        "Send", "Show", "Take", "Give", "SWMG_", "TUR_",
                        "PAZ_", "Add", "Adjust", "Ambient", "Aur", "Award",
                        "Bark", "Begin", "Cancel", "Create", "Disable", "End",
                        "Event", "Execute", "Location", "NoClicks", "Object",
                        "Queue", "Reveal", "Ship", "Signal", "Spawn", "Start",
                        "Surrender", "Switch", "Talent", "Yavin")):
        return "mediated engine effect"
    if name.startswith(("JSON", "DeleteJournal")):
        return "journal"
    if name.startswith(("Vector", "Angle", "Feet", "Yards", "Turns")):
        return "vector/math"
    if name in {"abs", "fabs", "acos", "asin", "atan", "cos", "sin", "tan",
                "log", "pow", "sqrt", "Random", "RandomName"}:
        return "math"
    if name.startswith(("String", "Insert", "IntTo", "FloatTo", "FindSub")):
        return "string"
    if name in {"PopUpGUIPanel", "PopUpDeathGUIPanel",
                "FloatingTextStrRefOnCreature",
                "FloatingTextStringOnCreature",
                "SuppressStatusSummaryEntry"}:
        return "UI/floating text"
    if name in {"MagicalEffect", "SupernaturalEffect", "ExtraordinaryEffect",
                "VersusAlignmentEffect", "VersusRacialTypeEffect",
                "VersusTrapEffect"}:
        return "effect wrapper"
    return "engine-mediated"


def check(specs: dict, unused_ids: list[int]) -> dict:
    by_id = {a["id"]: a for a in specs["actions"]}
    runtime = ActionRuntime(specs)
    vm = ncs_vm.VM([], specs, runtime=runtime)
    rows = []
    errors = []
    for aid in unused_ids:
        spec = by_id.get(aid)
        if spec is None:
            errors.append({"id": aid, "error": "missing spec"})
            continue
        args = []
        for p in spec["params"]:
            args.append([] if p["type"] == "action"
                        else ncs_vm.default_cells(p["type"]))
        before_specific = set(runtime.specific)
        before_generic = set(runtime.generic)
        before_default = set(runtime.default)
        try:
            cells = runtime.call(aid, args, vm)
            error = None
        except Exception as exc:  # noqa: BLE001 - one bad action must not stop the ledger
            cells = []
            error = f"{type(exc).__name__}: {exc}"
        want = 0 if spec["return"] == "void" else (
            3 if spec["return"] == "vector" else 1)
        if error is None and len(cells) != want:
            error = f"return cells {len(cells)} != {want}"
        name = spec["name"]
        if name in runtime.specific:
            handler = "specific"
        elif name in runtime.generic:
            handler = "generic"
        elif name in runtime.default:
            handler = "default"
        else:
            handler = "unknown"
        if error is None and handler == "default":
            error = "unclassified default handler"
        row = {
            "id": aid,
            "name": name,
            "return": spec["return"],
            "args": spec["args"],
            "handler": handler,
            "category": category(name),
            "return_cells": len(cells),
            "error": error,
        }
        rows.append(row)
        if error:
            errors.append({"id": aid, "name": name, "error": error})
        runtime.specific = before_specific
        runtime.generic = before_generic
        runtime.default = before_default
    counts = Counter(r["handler"] for r in rows)
    cats = Counter(r["category"] for r in rows)
    return {
        "schema": "kotor-s3-unused-actions/1",
        "build_sha256": specs.get("build_sha256"),
        "unused_count": len(unused_ids),
        "expected": EXPECTED_UNUSED,
        "checked": len(rows),
        "handlers": dict(sorted(counts.items())),
        "categories": dict(sorted(cats.items())),
        "errors": errors,
        "gate": {
            "all_306_checked": len(rows) == EXPECTED_UNUSED,
            "zero_errors": not errors,
            "zero_default": counts.get("default", 0) == 0,
            "every_action_typed": all(r["error"] is None for r in rows),
        },
        "actions": rows,
    }


def markdown(report: dict) -> str:
    g = report["gate"]
    status = "closed" if all(g.values()) else "open"
    lines = [
        "# 30 — Unused-action conformance (the 306 ids outside the shipped corpus)",
        "",
        f"Status: **{status}** — {report['checked']}/{report['expected']} unused action ids "
        f"invoked through the functional runtime for build SHA-256 "
        f"`{report['build_sha256']}`.",
        "",
        "## Gate",
        "",
        "| check | result |",
        "|---|---|",
        f"| Unused ids checked | **{report['checked']}/{report['expected']}** |",
        f"| Specific handlers | **{report['handlers'].get('specific', 0)}** |",
        f"| Generic semantic handlers | **{report['handlers'].get('generic', 0)}** |",
        f"| Unclassified/default | **{report['handlers'].get('default', 0)}** |",
        f"| Errors | **{len(report['errors'])}** |",
        f"| Gate | **{status}** |",
        "",
        "## Categories",
        "",
        "| category | actions |",
        "|---|---:|",
    ]
    for k, v in report["categories"].items():
        lines.append(f"| {k} | {v} |")
    lines += [
        "",
        "## Method",
        "",
        "- The corpus usage ledger (`artifacts/lanes/s1-04-scripts/action_usage_report.json`)",
        "  lists 466 actions used by the 8,666 shipped scripts; the other 306 are listed here.",
        "- For each unused id, a synthetic ACTION is built with the prototype's declared",
        "  parameter count and a typed default for each parameter (vector 3 cells, functor 0).",
        "- The action is dispatched through `tools/vm/actions_runtime.py`; the return cell count",
        "  is checked against the prototype (void 0, vector 3, everything else 1).",
        "- `specific` handlers implement the action directly; `generic` handlers apply a",
        "  documented semantic group (query, state/effect, action queue, UI, journal, audio,",
        "  minigame, math/string). `default` is the unclassified fallback and must be zero.",
        "- Behavioral conformance (prepared world, state-derived values, queue/mediation",
        "  records) is in `docs/recon/34_ACTION_BEHAVIOR.md`.",
        "",
        "## Full list",
        "",
        "| id | action | return | handler | category |",
        "|---:|---|---|---|---|",
    ]
    for r in report["actions"]:
        lines.append(f"| {r['id']} | {r['name']} | {r['return']} | "
                     f"{r['handler']} | {r['category']} |")
    lines += [
        "",
        "## Reproduce",
        "",
        "```bash",
        "python tools/vm/unused_actions.py \\",
        "  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \\",
        "  --usage artifacts/lanes/s1-04-scripts/action_usage_report.json \\",
        "  --out-json artifacts/lanes/s3-02-actions/unused_actions.json \\",
        "  --out-doc docs/recon/30_UNUSED_ACTIONS.md",
        "```",
        "",
    ]
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--specs", required=True)
    ap.add_argument("--usage", required=True)
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--out-doc", required=True)
    args = ap.parse_args(argv)
    specs = json.loads(Path(args.specs).read_text(encoding="utf-8"))
    usage = json.loads(Path(args.usage).read_text(encoding="utf-8"))
    unused_ids = usage["summary"]["actions_unused"]
    report = check(specs, unused_ids)
    out_json = Path(args.out_json)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    out_doc = Path(args.out_doc)
    out_doc.parent.mkdir(parents=True, exist_ok=True)
    out_doc.write_text(markdown(report) + "\n", encoding="utf-8")
    print(json.dumps({"checked": report["checked"],
                      "handlers": report["handlers"],
                      "errors": len(report["errors"]),
                      "gate": report["gate"]}, sort_keys=True))
    return 0 if all(report["gate"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
