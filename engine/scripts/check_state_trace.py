#!/usr/bin/env python3
"""kotor_state_trace: --state-trace writes a trace that parses with the shared record grammar and compares.

Runs the engine headless on end_m01aa (a short fixed-clock scenario) twice with --state-trace, then checks:
  1. the file follows the grammar of the state trace spec (header, TAB-separated records, field counts and
     number formats per record class, no empty values, no free text);
  2. the required records are there (mod ready, obj, cam, party, a settle anchor);
  3. the two runs write the same records (frame and wall time aside);
  4. compare.py of the trace with itself has no FAIL, and a copy with the hit points of one creature edited
     FAILs the obj class on that creature (the tolerances bite).

compare.py is the retail-side lane's script: STATE_TRACE_COMPARE names it, else the copy beside this file
(state_trace_compare.py) is used.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

FLOAT = re.compile(r"^-?\d+\.\d{3}$")
INT = re.compile(r"^-?\d+$")
HEXID = re.compile(r"^0x[0-9a-f]+$")
RESREF = re.compile(r"^[A-Za-z0-9_.\-]+$")

# kind -> (minimum field count, per-field checks {index: regex})
GRAMMAR = {
    "mod": (2, {}),
    "area": (1, {}),
    "pause": (2, {0: INT, 1: HEXID}),
    "party": (2, {}),
    "obj": (14, {1: HEXID, 4: FLOAT, 5: FLOAT, 6: FLOAT, 7: FLOAT, 8: FLOAT, 9: INT}),
    "obj_del": (2, {1: HEXID}),
    "clip": (3, {}),
    "cam": (12, {i: FLOAT for i in range(12)}),
    "camm": (1, {}),
    "sel": (5, {2: FLOAT, 3: FLOAT, 4: FLOAT}),
    "gui_stack": (1, {}),
    "gui": (7, {}),
    "ctl": (8, {1: INT}),
    "dlg": (5, {}),
    "dlgcam": (6, {}),
    "snd": (11, {}),
    "act": (1, {0: INT}),
    "rnd": (3, {1: INT}),
    "shot": (2, {}),
}


def parse(path: Path) -> tuple[list[str], list[list[str]]]:
    """Returns (problems, records); a record is [frame, t_ms, gt, kind, fields...]."""
    problems: list[str] = []
    records: list[list[str]] = []
    raw = path.read_bytes()
    if b"\r" in raw:
        problems.append("file holds a CR")
    lines = raw.decode("utf-8").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    if not lines or lines[0] != "#kotor-state-trace\t1":
        problems.append("first line is not '#kotor-state-trace<TAB>1'")
    meta = {}
    for number, line in enumerate(lines, 1):
        if line.startswith("#"):
            cols = line.split("\t")
            if cols[0] == "#meta" and len(cols) >= 3:
                meta[cols[1]] = cols[2]
            continue
        cols = line.split("\t")
        if len(cols) < 5:
            problems.append(f"line {number}: fewer than 5 columns")
            continue
        if not INT.match(cols[0]) or not INT.match(cols[1]):
            problems.append(f"line {number}: frame or t_ms is not an integer")
        if cols[2] != "-" and not re.match(r"^\d+:\d+$", cols[2]):
            problems.append(f"line {number}: bad gt {cols[2]!r}")
        kind = cols[3]
        if kind not in GRAMMAR:
            problems.append(f"line {number}: unknown record class {kind!r}")
            continue
        fields = cols[4:]
        need, checks = GRAMMAR[kind]
        if len(fields) < need:
            problems.append(f"line {number}: {kind} has {len(fields)} fields, needs {need}")
            continue
        for index, value in enumerate(fields):
            if value == "":
                problems.append(f"line {number}: {kind} field {index} is empty (an empty value is '-')")
        for index, pattern in checks.items():
            if fields[index] != "-" and not pattern.match(fields[index]):
                problems.append(f"line {number}: {kind} field {index} {fields[index]!r} breaks the number format")
        if kind == "dlg" and fields[0] == "line":
            # line dlgResref n owner speaker listener angle camId nAnims textHash raw: tags, numbers and a hash only
            if len(fields) < 10:
                problems.append(f"line {number}: dlg line has {len(fields)} fields, needs 10")
            else:
                for index in (2, 6, 7, 8):
                    if not INT.match(fields[index]):
                        problems.append(f"line {number}: dlg field {index} is not a number")
                if fields[9] != "-" and not re.match(r"^[0-9a-f]{8}$", fields[9]):
                    problems.append(f"line {number}: dlg text hash is not 8 hex digits")
        records.append(cols)
    for required in ("source", "build", "cfg"):
        if required not in meta:
            problems.append(f"missing #meta {required}")
    if meta.get("source") != "engine":
        problems.append("#meta source is not engine")
    return problems, records


def run_engine(args, trace: Path, name: str) -> int:
    profile = Path(args.out) / name
    profile.mkdir(parents=True, exist_ok=True)
    cmd = [args.exe, "--module", "end_m01aa", "--null", "--headless", "--frames", str(args.frames),
           "--fixed-dt", "0.0166667", "--no-audio", "--no-movies",
           "--state-trace", str(trace), "--saves-dir", str(profile / "saves"),
           "--settings-file", str(profile / "settings.ini")]
    if args.install:
        cmd += ["--install", args.install]
    env = dict(os.environ, SDL_VIDEODRIVER="dummy")
    done = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout, env=env, stdin=subprocess.DEVNULL)
    if done.returncode != 0:
        print(done.stdout[-2000:])
        print(done.stderr[-2000:])
    return done.returncode


def stable(records: list[list[str]]) -> list[tuple]:
    """Records without frame and wall time: what two runs of one scenario must agree on."""
    return [tuple(r[2:]) for r in records]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--install", default="")
    ap.add_argument("--out", required=True)
    ap.add_argument("--frames", type=int, default=160)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--compare", default=os.environ.get("STATE_TRACE_COMPARE", ""))
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    compare = Path(args.compare) if args.compare else Path(__file__).resolve().parent / "state_trace_compare.py"
    failures: list[str] = []

    traces = [out / "run_a.tsv", out / "run_b.tsv"]
    for index, trace in enumerate(traces):
        if trace.exists():
            trace.unlink()
        code = run_engine(args, trace, f"run_{index}")
        if code != 0 or not trace.exists():
            print(f"FAIL: engine exit {code}, trace {'present' if trace.exists() else 'missing'}")
            return 1

    problems, records = parse(traces[0])
    failures += problems[:20]
    kinds = {}
    for record in records:
        kinds[record[3]] = kinds.get(record[3], 0) + 1
    print("records:", ", ".join(f"{k}={v}" for k, v in sorted(kinds.items())))
    ready = [r for r in records if r[3] == "mod" and r[4] == "ready" and r[5].lower() == "end_m01aa"]
    if not ready:
        failures.append("no 'mod ready end_m01aa' record")
    for need in ("obj", "cam", "party"):
        if kinds.get(need, 0) == 0:
            failures.append(f"no {need} record")
    if not any(r[3] == "shot" and r[4].lower() == "settle:end_m01aa" for r in records):
        failures.append("no settle:end_m01aa anchor (shot record)")
    if any(r[3] == "obj" and r[2] == "-" for r in records) is False:
        pass  # gt is '-' in the engine trace by design

    problems_b, records_b = parse(traces[1])
    failures += problems_b[:5]
    if stable(records) != stable(records_b):
        first = next((i for i, (a, b) in enumerate(zip(stable(records), stable(records_b))) if a != b),
                     min(len(records), len(records_b)))
        failures.append(f"two runs of one scenario differ at record {first} (of {len(records)}/{len(records_b)})")

    if not compare.exists():
        failures.append(f"compare script missing: {compare}")
    else:
        same = subprocess.run([sys.executable, "-I", str(compare), str(traces[0]), str(traces[0])],
                              capture_output=True, text=True, timeout=300)
        print(same.stdout.strip())
        if same.returncode != 0 or re.search(r"^FAIL", same.stdout, re.M):
            failures.append("compare.py of the trace with itself reports a FAIL")
        if not re.search(r"^PASS +obj", same.stdout, re.M):
            failures.append("compare.py of the trace with itself does not PASS the obj class")
        # One creature's hit points edited in every record: obj must FAIL on that creature.
        victim = next((r[4] for r in records if r[3] == "obj" and r[6] == "creature"), None)
        if victim is None:
            failures.append("no creature in the trace to edit")
        else:
            edited = out / "edited.tsv"
            lines = []
            for line in traces[0].read_text(encoding="utf-8").split("\n"):
                cols = line.split("\t")
                if len(cols) >= 18 and cols[3] == "obj" and cols[4] == victim:
                    cols[13] = str(int(cols[13]) + 7) if INT.match(cols[13]) else "7"
                    line = "\t".join(cols)
                lines.append(line)
            edited.write_text("\n".join(lines), encoding="utf-8")
            bad = subprocess.run([sys.executable, "-I", str(compare), str(traces[0]), str(edited)],
                                 capture_output=True, text=True, timeout=300)
            hit = re.search(r"^FAIL +obj.*", bad.stdout, re.M)
            if hit is None or victim.split("#")[0] not in hit.group(0) or '"hp"' not in hit.group(0):
                failures.append(f"an edited hp of {victim} did not FAIL obj with that key and field: {bad.stdout[-500:]}")
            else:
                print("negative check ok:", hit.group(0)[:160])

    # The segment-aligned divergence report (state_trace_diff.py): a trace against itself has no divergence class, and the
    # hit-point edit shows up as one obj hp class.
    diff_tool = Path(__file__).resolve().parent / "state_trace_diff.py"
    if not diff_tool.exists():
        failures.append(f"diff script missing: {diff_tool}")
    else:
        same_out = out / "diff_same"
        done = subprocess.run([sys.executable, "-I", str(diff_tool), str(traces[0]), str(traces[0]), "--sample", "same", "--out", str(same_out)],
                              capture_output=True, text=True, timeout=300)
        if done.returncode != 0 or "0 divergence classes" not in done.stdout:
            failures.append("state_trace_diff.py of the trace with itself reports divergence classes: " + done.stdout[-400:])
        edited = out / "edited.tsv"
        if edited.exists():
            edit_out = out / "diff_edit"
            subprocess.run([sys.executable, "-I", str(diff_tool), str(traces[0]), str(edited), "--sample", "edit", "--out", str(edit_out)],
                           capture_output=True, text=True, timeout=300)
            table = (edit_out / "DIFF.tsv").read_text(encoding="utf-8") if (edit_out / "DIFF.tsv").exists() else ""
            if not re.search(r"^\d+\tobj\thp\t", table, re.M):
                failures.append("state_trace_diff.py did not report the edited hit points as an obj hp class")
            else:
                print("diff negative check ok: obj hp class reported")

    if failures:
        for text in failures:
            print("FAIL:", text)
        return 1
    print("PASS: state trace grammar, scenario, determinism and compare")
    return 0


if __name__ == "__main__":
    sys.exit(main())
