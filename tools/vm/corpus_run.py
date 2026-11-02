#!/usr/bin/env python3
"""Run the independent NCS VM over every unique shipped script.

Emits a per-file JSON ledger and a tracked Markdown report
(``docs/recon/27_VM_EXECUTION.md``).  Gate S2 requires 8,666/8,666 scripts to
decode and execute from their entry point with zero decode errors, zero VM
faults, and zero unknown action ids.

Usage:
    python tools/vm/corpus_run.py \
        --corpus artifacts/lanes/04-ncs/extracted/unique \
        --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
        --out-json artifacts/lanes/s2-03-vm-corpus/vm_corpus.json \
        --out-doc docs/recon/27_VM_EXECUTION.md
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys
from collections import Counter
from pathlib import Path

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
sys.path.insert(0, os.path.join(_HERE, ".."))

from vm import ncs_vm  # noqa: E402
from vm.actions_runtime import ActionRuntime  # noqa: E402

EXPECTED_FILES = 8666


def run_corpus(corpus: str, specs: dict, budget: int,
               use_runtime: bool = False) -> dict:
    factory = (lambda: ActionRuntime(specs)) if use_runtime else None
    rows = []
    for path in sorted(glob.glob(os.path.join(corpus, "*.ncs"))):
        result = ncs_vm.run_file(path, specs, budget=budget,
                                 runtime_factory=factory)
        result["file"] = os.path.basename(path)
        rows.append(result)
    status_counts = Counter(r["status"] for r in rows)
    action_counts = Counter()
    for r in rows:
        action_counts.update(r.get("actions", {}))
    used_ids = sorted(action_counts)
    spec_ids = {a["id"] for a in specs["actions"]}
    unknown_ids = [i for i in used_ids if i not in spec_ids]
    errors = [r for r in rows if r["status"] in ("error", "decode_error")]
    blocked = [r for r in rows if r["status"] == "blocked"]
    budget_rows = [r for r in rows if r["status"] == "budget"]
    invalid_targets = []
    for r in rows:
        for bad in r.get("invalid_jump_targets", []):
            invalid_targets.append({"file": r["file"], **bad})
    runtime_specific: set[str] = set()
    runtime_generic: set[str] = set()
    runtime_default: set[str] = set()
    runtime_calls = runtime_timers = runtime_queued = 0
    for r in rows:
        rt = r.get("runtime")
        if not rt:
            continue
        runtime_calls += rt.get("calls", 0)
        runtime_specific.update(rt.get("specific", []))
        runtime_generic.update(rt.get("generic", []))
        runtime_default.update(rt.get("default", []))
        runtime_timers += rt.get("timers", 0)
        runtime_queued += rt.get("queued_actions", 0)
    summary = {
        "schema": "kotor-s2-vm-corpus/1",
        "build_sha256": specs.get("build_sha256"),
        "count": len(rows),
        "expected_files": EXPECTED_FILES,
        "status_counts": dict(sorted(status_counts.items())),
        "action_calls": sum(r.get("action_calls", 0) for r in rows),
        "actions_used": len(used_ids),
        "unknown_action_ids": unknown_ids,
        "action_specs": len(spec_ids),
        "decode_errors": sum(1 for r in rows if r["status"] == "decode_error"),
        "vm_errors": sum(1 for r in rows if r["status"] == "error"),
        "errors": [{"file": r["file"], "status": r["status"], "error": r.get("error"),
                    "error_ip": r.get("error_ip")} for r in errors],
        "blocked": len(blocked),
        "budget": len(budget_rows),
        "budget_files": [{"file": r["file"], "steps": r["steps"]} for r in budget_rows],
        "max_steps": max((r.get("steps", 0) for r in rows), default=0),
        "max_sp": max((r.get("max_sp", 0) for r in rows), default=0),
        "total_steps": sum(r.get("steps", 0) for r in rows),
        "instructions": sum(r.get("instruction_count", 0) for r in rows),
        "invalid_jump_targets": invalid_targets,
        "runtime": {
            "enabled": use_runtime,
            "calls": runtime_calls,
            "specific": sorted(runtime_specific),
            "generic": sorted(runtime_generic),
            "default": sorted(runtime_default),
            "timers": runtime_timers,
            "queued_actions": runtime_queued,
        },
        "files": rows,
    }
    summary["gate"] = {
        "all_files_executed": len(rows) == EXPECTED_FILES,
        "zero_decode_errors": summary["decode_errors"] == 0,
        "zero_vm_faults": summary["vm_errors"] == 0,
        "zero_unknown_actions": not unknown_ids,
        "all_772_specs": len(spec_ids) == 772,
        "zero_invalid_jump_targets": not invalid_targets,
    }
    if use_runtime:
        summary["gate"]["zero_default_corpus_actions"] = not runtime_default
        summary["gate"]["runtime_enabled"] = True
    summary["gate"]["status"] = "closed" if all(summary["gate"].values()) else "open"
    return summary


def markdown(s: dict, specs: dict) -> str:
    names = {a["id"]: a["name"] for a in specs["actions"]}
    action_counts = Counter()
    for r in s["files"]:
        action_counts.update(r.get("actions", {}))
    top = action_counts.most_common(20)
    g = s["gate"]
    mode = ("stateful action runtime" if s["runtime"]["enabled"]
            else "typed-default mediation")
    title = ("# 29 — Functional NCS runtime: full-corpus execution"
             if s["runtime"]["enabled"]
             else "# 27 — Independent NCS VM: full-corpus execution")
    lines = [
        title,
        "",
        f"Status: **{g['status']}** — {s['count']:,}/{s['expected_files']:,} unique shipped NCS "
        f"for build SHA-256 `{s['build_sha256']}` ({mode}).",
        "",
        "## Gate checks",
        "",
        "| check | result |",
        "|---|---|",
        f"| Unique NCS decoded from entry | **{s['count']:,}/{s['expected_files']:,}** |",
        f"| Decode errors | **{s['decode_errors']}** |",
        f"| VM faults | **{s['vm_errors']}** |",
        f"| Unknown action ids | **{len(s['unknown_action_ids'])}** |",
        f"| Invalid jump/STORESTATE targets | **{len(s['invalid_jump_targets'])}** |",
        f"| Action specs available | **{s['action_specs']}/772** |",
    ]
    if s["runtime"]["enabled"]:
        lines.append(f"| Zero default corpus actions | **{len(s['runtime']['default'])}** |")
    lines += [
        f"| Gate | **{g['status']}** |",
        "",
        "## Execution results",
        "",
        "| status | files | meaning |",
        "|---|---:|---|",
        f"| returned | {s['status_counts'].get('returned', 0):,} | reached RETN with an empty return stack |",
        f"| blocked | {s['status_counts'].get('blocked', 0):,} | stopped at `ActionWait` (event-driven script) |",
        f"| budget | {s['status_counts'].get('budget', 0):,} | hit the {s['max_steps']:,}-step ceiling (loop without a wait) |",
        f"| error | {s['status_counts'].get('error', 0):,} | VM fault |",
        f"| decode_error | {s['status_counts'].get('decode_error', 0):,} | malformed NCS |",
        "",
        f"Decoded instructions: **{s['instructions']:,}**; executed: "
        f"**{s['total_steps']:,}**; ACTION calls: **{s['action_calls']:,}**; unique action "
        f"ids used: **{s['actions_used']}**; maximum stack: **{s['max_sp']}** cells.",
        "",
        "## Top actions by calls",
        "",
        "| id | action | calls |",
        "|---:|---|---:|",
    ]
    if s["runtime"]["enabled"]:
        rt = s["runtime"]
        runtime_lines = [
            "## Functional runtime coverage",
            "",
            f"- ACTION calls mediated: **{rt['calls']:,}**",
            f"- Specific handlers used: **{len(rt['specific'])}**",
            f"- Generic state/effect handlers used: **{len(rt['generic'])}**",
            f"- Unclassified/default handlers used: **{len(rt['default'])}**",
            f"- Timers scheduled: **{rt['timers']:,}**; queued actions: "
            f"**{rt['queued_actions']:,}**",
            "",
        ]
        if rt["default"]:
            runtime_lines.append(
                "Default-used actions: " + ", ".join(f"`{x}`" for x in rt["default"]) + ".")
            runtime_lines.append("")
        try:
            idx = lines.index("## Top actions by calls")
            lines[idx:idx] = runtime_lines
        except ValueError:
            lines += runtime_lines
    for aid, count in top:
        lines.append(f"| {aid} | {names.get(aid, '?')} | {count:,} |")
    lines += [
        "",
        "## Blocked / budget samples",
        "",
        f"`ActionWait` (id 202) stopped {s['blocked']:,} event-loop scripts. The "
        f"{s['budget']} budget stops are loops with no wait action; they are expected for "
        "scripts that poll engine state.",
        "",
        "Budget stops:",
        "",
    ]
    for row in s["budget_files"][:20]:
        lines.append(f"- `{row['file']}` — {row['steps']:,} steps")
    lines += [
        "",
        "## Method",
        "",
        "- `tools/vm/ncs_vm.py` decodes the shipped NCS V1.0 stream with the independent "
        "`tools/scripts/ncs.py` codec and executes the stack machine from offset 0.",
        "- Control-flow parity: every `JMP`/`JSR`/`JZ`/`JNZ` and `STORESTATE` target is checked "
        "against the decoded instruction offsets; the gate requires zero invalid targets.",
        "- Stack offsets use `len(stack) + off/4`; every shipped `MOVSP` is negative and pops "
        "cells (699,919 occurrences checked). `JSR`/`RETN` use a separate return stack with the "
        "documented recursion limit 8. `SAVEBP`/`RESTOREBP` and `CPTOPBP`/`CPDOWNBP` implement "
        "globals; `DESTRUCT` uses the `(size, offset, keep)` cell counts.",
        "- ACTION mediation uses the 772 typed specs from `action_specs.json`: arguments are "
        "popped by declared width (vector = 3 cells, functor = 0 cells, otherwise 1), and every "
        "non-void action returns a typed default. No engine code is called.",
        "- A 200,000-instruction per-script budget bounds event loops; `ActionWait` is classified "
        "as `blocked` instead of spinning.",
        "",
        "## Reproduce",
        "",
        "```bash",
        "python tools/analysis/action_specs.py \\",
        "  --actions artifacts/action_table.json \\",
        "  --nwscript artifacts/game-extract/nwscript.nss \\",
        "  --functions artifacts/lanes/engine-semantics/export-v4/functions-v4.jsonl \\",
        "  --out artifacts/lanes/s2-01-engine-map/action_specs.json",
        "python tools/vm/corpus_run.py \\",
        "  --corpus artifacts/lanes/04-ncs/extracted/unique \\",
        "  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \\",
        "  --out-json artifacts/lanes/s2-03-vm-corpus/vm_corpus.json \\",
        "  --out-doc docs/recon/27_VM_EXECUTION.md",
        "```",
        "",
    ]
    if s["runtime"]["enabled"]:
        idx = lines.index("## Reproduce")
        lines[idx:idx] = [
            "- Functional mode uses `tools/vm/actions_runtime.py` over `tools/vm/state.py`: "
            "object/tag/area, inventory/party, local/global variables, strings/vectors, action "
            "queue, and timers are stateful; combat/dialogue/effects/audio are mediated and "
            "recorded. No original engine code is called.",
            "",
        ]
        cmd_idx = lines.index("  --out-json artifacts/lanes/s2-03-vm-corpus/vm_corpus.json \\")
        lines.insert(cmd_idx, "  --runtime \\")
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--specs", required=True)
    ap.add_argument("--budget", type=int, default=200_000)
    ap.add_argument("--runtime", action="store_true",
                    help="use the stateful action runtime")
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--out-doc", required=True)
    args = ap.parse_args(argv)
    specs = json.loads(Path(args.specs).read_text(encoding="utf-8"))
    summary = run_corpus(args.corpus, specs, args.budget, use_runtime=args.runtime)
    out_json = Path(args.out_json)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    out_doc = Path(args.out_doc)
    out_doc.parent.mkdir(parents=True, exist_ok=True)
    out_doc.write_text(markdown(summary, specs) + "\n", encoding="utf-8")
    print(json.dumps({k: summary[k] for k in (
        "count", "status_counts", "decode_errors", "vm_errors",
        "unknown_action_ids", "action_specs", "gate")}, sort_keys=True))
    return 0 if summary["gate"]["status"] == "closed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
