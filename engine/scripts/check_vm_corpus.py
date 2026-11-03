#!/usr/bin/env python3
"""Compare the engine's native NCS VM corpus run with the verified oracle.

This is a verification script only. The engine produces JSONL directly from its
own decoder/VM; this script diffs it against the Sprint 2 typed-default oracle
and (optionally) the Sprint 3 functional oracle.

    python engine/scripts/check_vm_corpus.py \
      --engine engine/build/msvc-release/kotor_engine.exe \
      --corpus artifacts/lanes/04-ncs/extracted/unique \
      --default-oracle artifacts/lanes/s2-03-vm-corpus/vm_corpus.json \
      --functional-oracle artifacts/lanes/s3-03-corpus/functional_corpus.json \
      --out artifacts/lanes/s6-01-vm/parity.json
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


def load_jsonl(path: Path) -> list[dict]:
    rows = []
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def compare_scalar(failures: list, file: str, key: str, got, want) -> None:
    if got != want:
        failures.append({"file": file, "field": key, "got": got, "want": want})


def run_engine(engine: Path, corpus: Path, out: Path, functional: bool, limit: int) -> dict:
    args = [str(engine), "--vm-corpus", str(corpus), "--vm-out", str(out)]
    if functional:
        args.append("--vm-functional")
    if limit:
        args += ["--vm-limit", str(limit)]
    result = subprocess.run(args, capture_output=True, text=True, check=False)
    return {
        "cmd": args,
        "returncode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }


def compare_default(rows: list[dict], oracle: dict) -> tuple[list[dict], dict]:
    want = {f["file"]: f for f in oracle["files"]}
    failures: list[dict] = []
    fields = (
        "status",
        "steps",
        "max_sp",
        "actions",
        "top",
        "invalid_jump_targets",
        "error",
        "error_ip",
        "blocked_action",
        "instruction_count",
    )
    for row in rows:
        base = want.get(row["file"])
        if base is None:
            failures.append({"file": row["file"], "field": "presence", "got": "extra", "want": None})
            continue
        for field in fields:
            compare_scalar(failures, row["file"], field, row.get(field), base.get(field))
    got_files = {r["file"] for r in rows}
    missing = sorted(set(want) - got_files)
    for name in missing:
        failures.append({"file": name, "field": "presence", "got": None, "want": "present"})
    return failures, {
        "oracle_files": len(want),
        "engine_files": len(rows),
        "missing": len(missing),
    }


def compare_functional(rows: list[dict], oracle: dict) -> tuple[list[dict], dict]:
    want = {f["file"]: f for f in oracle["files"]}
    failures: list[dict] = []
    fields = (
        "status",
        "steps",
        "max_sp",
        "actions",
        "action_calls",
        "top",
        "invalid_jump_targets",
        "error",
        "error_ip",
        "blocked_action",
    )
    for row in rows:
        base = want.get(row["file"])
        if base is None:
            continue
        for field in fields:
            compare_scalar(failures, row["file"], field, row.get(field), base.get(field))
        runtime = row.get("runtime") or {}
        expected = base.get("runtime") or {}
        compare_scalar(failures, row["file"], "runtime.specific", runtime.get("specific"),
                       expected.get("specific"))
        compare_scalar(failures, row["file"], "runtime.generic", runtime.get("generic"),
                       expected.get("generic"))
        compare_scalar(failures, row["file"], "runtime.default", runtime.get("default"),
                       expected.get("default"))
        compare_scalar(failures, row["file"], "runtime.calls", runtime.get("calls"),
                       expected.get("calls"))
        compare_scalar(failures, row["file"], "runtime.timers", runtime.get("timers"),
                       expected.get("timers"))
        compare_scalar(failures, row["file"], "runtime.queued_actions",
                       runtime.get("queued_actions"), expected.get("queued_actions"))
        compare_scalar(failures, row["file"], "runtime.world", runtime.get("world"),
                       expected.get("world"))
    return failures, {"oracle_files": len(want), "engine_files": len(rows)}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--default-oracle")
    parser.add_argument("--functional-oracle")
    parser.add_argument("--out", required=True)
    parser.add_argument("--workdir", default=None)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--functional", action="store_true")
    parser.add_argument("--keep-jsonl", default=None)
    args = parser.parse_args(argv)

    root = Path(args.workdir) if args.workdir else Path.cwd()
    engine = Path(args.engine)
    corpus = Path(args.corpus)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    jsonl = Path(args.keep_jsonl) if args.keep_jsonl else out.with_suffix(".jsonl")
    jsonl.parent.mkdir(parents=True, exist_ok=True)

    report: dict = {
        "schema": "kotor.vm-parity/1",
        "engine": str(engine),
        "corpus": str(corpus),
        "functional": args.functional,
        "limit": args.limit,
    }
    failures: list[dict] = []

    default_rows: list[dict] = []
    if args.default_oracle:
        run = run_engine(engine, corpus, jsonl, False, args.limit)
        report["default_run"] = run
        if run["returncode"] != 0:
            print(run["stdout"])
            print(run["stderr"], file=sys.stderr)
            return 2
        default_rows = load_jsonl(jsonl)
        oracle = json.loads(Path(args.default_oracle).read_text(encoding="utf-8"))
        diff, stats = compare_default(default_rows, oracle)
        failures.extend(diff)
        report["default"] = {
            "oracle": args.default_oracle,
            "mismatches": len(diff),
            "examples": diff[:20],
            **stats,
        }
        print(
            "default: files=%d oracle=%d mismatches=%d"
            % (len(default_rows), stats["oracle_files"], len(diff))
        )

    if args.functional or args.functional_oracle:
        run = run_engine(engine, corpus, jsonl, True, args.limit)
        report["functional_run"] = run
        if run["returncode"] != 0:
            print(run["stdout"])
            print(run["stderr"], file=sys.stderr)
            return 2
        rows = load_jsonl(jsonl)
        stats = {"engine_files": len(rows)}
        diff: list[dict] = []
        if args.functional_oracle:
            oracle = json.loads(Path(args.functional_oracle).read_text(encoding="utf-8"))
            diff, stats = compare_functional(rows, oracle)
        failures.extend(diff)
        report["functional"] = {
            "oracle": args.functional_oracle,
            "mismatches": len(diff),
            "examples": diff[:20],
            **stats,
        }
        statuses: dict[str, int] = {}
        action_calls = 0
        for row in rows:
            statuses[row["status"]] = statuses.get(row["status"], 0) + 1
            action_calls += row.get("action_calls", 0)
        report["functional"]["statuses"] = statuses
        report["functional"]["action_calls"] = action_calls
        print(
            "functional: files=%d statuses=%s action_calls=%d mismatches=%d"
            % (len(rows), statuses, action_calls, len(diff))
        )

    report["pass"] = not failures
    report["failures"] = failures[:100]
    out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("report:", out)
    if failures:
        for failure in failures[:20]:
            print("MISMATCH", failure)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
