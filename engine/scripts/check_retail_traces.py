#!/usr/bin/env python3
"""Retail trace action coverage for the recorded s4 semantic traces.

The s4 traces are retail-engine action/command/exec/frame events recorded from
live sessions. This gate extracts every action id the retail engine executed
and asserts that the engine's action table implements it. It is the trace-level
counterpart to the VM corpus behaviour oracle.
"""
from __future__ import annotations

import argparse
import collections
import json
import re
from pathlib import Path


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace-dir", required=True)
    parser.add_argument("--actions", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    trace_dir = Path(args.trace_dir)
    actions_file = Path(args.actions)
    observed: set[int] = set()
    kinds: collections.Counter[str] = collections.Counter()
    events = 0
    for path in sorted(trace_dir.glob("*-semantic.jsonl")):
        with path.open(encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                payload = json.loads(line)
                events += 1
                kind = payload.get("kind", "")
                kinds[kind] += 1
                if kind == "kotor.action":
                    action = payload.get("payload", {}).get("action")
                    if isinstance(action, int):
                        observed.add(action)
    table = actions_file.read_text(encoding="utf-8", errors="replace")
    implemented = {int(value) for value in re.findall(r"ACTION\((\d+),", table)}
    missing = sorted(observed - implemented)
    report = {
        "schema": "kotor.retail-trace-coverage/1",
        "trace_dir": str(trace_dir),
        "events": events,
        "kinds": dict(sorted(kinds.items())),
        "observed_actions": sorted(observed),
        "observed_action_count": len(observed),
        "implemented_action_count": len(implemented),
        "missing_actions": missing,
        "pass": not missing and len(observed) > 0,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                   encoding="utf-8")
    print(json.dumps({key: report[key] for key in
                      ("events", "observed_action_count", "implemented_action_count",
                       "missing_actions", "pass")}, sort_keys=True))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
