#!/usr/bin/env python3
"""Run the retail-vs-engine semantic differential as one CTest target.

Records one engine trace, one drained functional VM corpus, then diffs both
against every supplied recorded retail trace.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str]) -> int:
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=False).returncode


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--install", required=True)
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--retail-trace", action="append", required=True)
    parser.add_argument("--module-usage", required=True)
    parser.add_argument("--action-table", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--module", default="end_m01aa")
    args = parser.parse_args(argv)

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    engine_trace = out / "engine-semantic.jsonl"
    vm_actions = out / "vm-actions.jsonl"

    code = run([args.engine, "--module", args.module,
                "--retail-differential-test", "--engine-trace", str(engine_trace),
                "--install", args.install, "--null", "--headless"])
    if code != 0:
        return code
    code = run([args.engine, "--vm-corpus", args.corpus, "--vm-out", str(vm_actions),
                "--vm-functional", "--vm-drain", "--null", "--headless"])
    if code != 0:
        return code

    reports = []
    for trace in args.retail_trace:
        trace_path = Path(trace)
        report = out / f"retail-differential-{trace_path.stem}.json"
        code = run([sys.executable,
                    str(Path(__file__).with_name("check_retail_differential.py")),
                    "--engine-trace", str(engine_trace),
                    "--retail-trace", str(trace_path),
                    "--vm-coverage", str(vm_actions),
                    "--module-usage", args.module_usage,
                    "--module", args.module,
                    "--action-table", args.action_table,
                    "--expect-choices", "9",
                    "--expect-transitions", "1",
                    "--out", str(report)])
        reports.append({"trace": str(trace_path), "report": str(report),
                        "pass": code == 0})
        if code != 0:
            break

    aggregate = {
        "schema": "kotor.retail-differential-set/1",
        "engine_trace": str(engine_trace),
        "vm_coverage": str(vm_actions),
        "traces": reports,
        "pass": all(item["pass"] for item in reports),
    }
    summary = out / "retail-differential.json"
    summary.write_text(json.dumps(aggregate, indent=2, sort_keys=True) + "\n",
                       encoding="utf-8")
    print(json.dumps({"traces": len(reports),
                      "passed": sum(1 for item in reports if item["pass"]),
                      "pass": aggregate["pass"]}, sort_keys=True))
    return 0 if aggregate["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
