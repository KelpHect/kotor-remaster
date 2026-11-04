#!/usr/bin/env python3
"""Convert a live-hook events.jsonl into the semantic JSONL trace shape.

The s4 semantic traces use `kotor.action`, `kotor.command` and `kotor.exec`
rows. The live harness emits `vm_action`, `run_command` and `vm_exec` rows;
this converter maps them one-to-one so a newly recorded session can be fed to
`check_retail_differential.py` without the Hydralysis ingestion step.

    python tools/harness/semantic_from_events.py \
        --events artifacts/lanes/s4-trace/runs/<run>/events.jsonl \
        --out docs/evidence/s14/<run>-semantic.jsonl
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


MODULE_RE = re.compile(r"modules[\\/]([A-Za-z0-9_]+?)(?:_s)?\.(?:rim|rsv)", re.I)


def convert(events: Path, out: Path) -> dict:
    counts: dict[str, int] = {}
    modules: set[str] = set()
    written = 0
    with events.open(encoding="utf-8") as source, out.open(
            "w", encoding="utf-8", newline="\n") as target:
        rows = []
        for line in source:
            line = line.strip()
            if not line:
                continue
            event = json.loads(line)
            kind = event.get("f")
            counts[kind] = counts.get(kind, 0) + 1
            path = event.get("path", "")
            match = MODULE_RE.search(path) if path else None
            if match:
                modules.add(match.group(1).lower())
            payload = None
            row_kind = None
            if kind == "vm_action":
                row_kind = "kotor.action"
                payload = {"action": event.get("id"),
                           "hook": "script-action", "site": "script-action"}
            elif kind == "run_command":
                row_kind = "kotor.command"
                payload = {"command": event.get("cmd"),
                           "hook": "run-command", "site": "run-command"}
            elif kind == "vm_exec":
                row_kind = "kotor.exec"
                payload = {"program": event.get("prog"),
                           "length": event.get("len"),
                           "hook": "execute-code", "site": "execute-code"}
            if row_kind is None:
                continue
            rows.append({
                "kind": row_kind,
                "seq": event.get("seq"),
                "t_mono_ns": event.get("t", 0) * 1_000_000,
                "thread_id": event.get("tid"),
                "payload": payload,
                "callstack": None,
            })
        target.write(json.dumps({
            "kind": "header",
            "seq": 0,
            "payload": {
                "schema": "kotor.retail-semantic/1",
                "modules": sorted(modules),
            },
            "callstack": None,
        }, sort_keys=True) + "\n")
        for row in rows:
            target.write(json.dumps(row, sort_keys=True) + "\n")
        written = len(rows)
    return {"events": str(events), "out": str(out), "written": written,
            "modules": sorted(modules), "counts": counts}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--events", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    result = convert(Path(args.events), Path(args.out))
    print(json.dumps(result, sort_keys=True))
    return 0 if result["written"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
