#!/usr/bin/env python3
"""Run the manifest-driven retail-vs-engine semantic differential.

The manifest defines one engine trace per scenario (module start or save load)
and one recorded retail trace per path. Each retail trace is diffed against its
engine trace plus the drained functional VM corpus, with zero missing actions
required.
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


def resolve(root: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else root / path


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--install", required=True)
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--module-usage", required=True)
    parser.add_argument("--action-table", required=True)
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args(argv)

    root = Path(args.repo_root)
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    engines = manifest["engines"]
    traces = manifest["traces"]

    vm_actions = out / "vm-actions.jsonl"
    code = run([args.engine, "--vm-corpus", args.corpus, "--vm-out", str(vm_actions),
                "--vm-functional", "--vm-drain", "--null", "--headless"])
    if code != 0:
        return code

    engine_traces: dict[str, Path] = {}
    for name, spec in engines.items():
        trace = out / f"engine-semantic-{name}.jsonl"
        cmd = [args.engine, "--module", spec.get("start_module", "end_m01aa"),
               "--retail-differential-test", "--engine-trace", str(trace),
               "--install", args.install, "--null", "--headless"]
        if spec.get("save"):
            cmd += ["--save-fixture", str(resolve(root, spec["save"]))]
        elif spec.get("module_trace"):
            cmd += ["--module-trace"]
        code = run(cmd)
        if code != 0:
            return code
        engine_traces[name] = trace

    reports = []
    for trace in traces:
        engine = trace["engine"]
        spec = engines[engine]
        report = out / f"retail-differential-{trace['id']}.json"
        code = run([sys.executable,
                    str(Path(__file__).with_name("check_retail_differential.py")),
                    "--engine-trace", str(engine_traces[engine]),
                    "--retail-trace", str(resolve(root, trace["retail"])),
                    "--vm-coverage", str(vm_actions),
                    "--module-usage", args.module_usage,
                    "--module", trace["module"],
                    "--action-table", args.action_table,
                    "--expect-choices", str(spec.get("expect_choices", 0)),
                    "--expect-transitions", str(spec.get("expect_transitions", 0)),
                    "--out", str(report)])
        reports.append({"id": trace["id"], "engine": engine,
                        "retail": trace["retail"], "report": str(report),
                        "pass": code == 0})
        if code != 0:
            break

    aggregate = {
        "schema": "kotor.retail-differential-set/1",
        "manifest": str(args.manifest),
        "engines": {name: str(path) for name, path in engine_traces.items()},
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
