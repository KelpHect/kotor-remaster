#!/usr/bin/env python3
"""Run the retail-vs-engine semantic differential as one CTest target."""
from __future__ import annotations

import argparse
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
    parser.add_argument("--retail-trace", required=True)
    parser.add_argument("--module-usage", required=True)
    parser.add_argument("--action-table", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--module", default="end_m01aa")
    args = parser.parse_args(argv)

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    engine_trace = out / "engine-semantic.jsonl"
    vm_actions = out / "vm-actions.jsonl"
    report = out / "retail-differential.json"

    code = run([args.engine, "--module", args.module,
                "--retail-differential-test", "--engine-trace", str(engine_trace),
                "--install", args.install, "--null", "--headless"])
    if code != 0:
        return code
    code = run([args.engine, "--vm-corpus", args.corpus, "--vm-out", str(vm_actions),
                "--vm-functional", "--vm-drain", "--null", "--headless"])
    if code != 0:
        return code
    return run([sys.executable, str(Path(__file__).with_name("check_retail_differential.py")),
                "--engine-trace", str(engine_trace),
                "--retail-trace", args.retail_trace,
                "--vm-coverage", str(vm_actions),
                "--module-usage", args.module_usage,
                "--module", args.module,
                "--action-table", args.action_table,
                "--expect-choices", "9",
                "--expect-transitions", "1",
                "--out", str(report)])


if __name__ == "__main__":
    raise SystemExit(main())
