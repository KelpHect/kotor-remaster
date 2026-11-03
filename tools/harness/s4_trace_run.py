#!/usr/bin/env python3
"""Sprint 4 live trace of KotOR under the proven lane-06 harness.

Runs the harness (which drives the real game to gameplay), attaches the Hydralysis semantic trace to the
harness-owned process with the build-bound KotOR profile, tails the recorder segment into the trace session
through the live pump, and records a timeline plus the final query. The evidence lands in
`artifacts/lanes/s4-trace/runs/<label>/` and `artifacts/lanes/s4-trace/<label>.log`.

Must run elevated (the game under the Mesa profile only accepts input at the same integrity level).
UAC is disabled on this host; `run_elevated.ps1` starts this script with `-Verb RunAs`.

    python tools/harness/s4_trace_run.py --label s4-kotor-01
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LANE = REPO / "artifacts" / "lanes" / "s4-trace"
S1 = REPO / "artifacts" / "lanes" / "s1-06-dynamic"
PROFILE = S1 / "profile"
FALLBACK_PROFILE = LANE / "profile"
TRACE_PROFILE = REPO / "tools" / "traces" / "kotor" / "swkotor-1.0.3.toml"
HARNESS = REPO / "tools" / "harness" / "sprint1_dynamic_run.py"
LIVE_HOOKS = REPO / "tools" / "harness" / "live_hooks.js"
SCENARIO = S1 / "scenarios" / "hw-startgame6-20261102.json"
PATCHED_SHA = "280ba024d7410933ef1dad5ed829e9bef428c07e7ae47d8a6ad7a3a9420d49e3"
BUILD_SHA = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6"


def _json(path: Path, default=None):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def _kinds(rows: list[dict]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for row in rows:
        kind = str(row.get("kind") or "?")
        counts[kind] = counts.get(kind, 0) + 1
    return dict(sorted(counts.items(), key=lambda item: (-item[1], item[0])))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--label", required=True)
    ap.add_argument("--scenario", default=str(SCENARIO))
    ap.add_argument("--profile", default=str(PROFILE))
    ap.add_argument("--trace-profile", default=str(TRACE_PROFILE))
    ap.add_argument("--hook", default=str(LIVE_HOOKS))
    ap.add_argument("--timeout", type=float, default=360.0, help="seconds to wait for the harness scenario")
    ap.add_argument("--poll", type=float, default=5.0, help="seconds between status snapshots")
    ap.add_argument("--no-trace", action="store_true", help="drive the harness only, without attaching the semantic trace")
    ap.add_argument("--grace", type=float, default=10.0,
                    help="seconds to keep recording after the first kotor.exec, then stop while the game is alive")
    ap.add_argument("--no-early-stop", action="store_true",
                    help="keep recording until the harness exits (may lose the final producer records)")
    args = ap.parse_args()

    profile = Path(args.profile)
    if not (profile / "swkotor.exe").is_file():
        print(f"profile exe missing: {profile / 'swkotor.exe'}", file=sys.stderr)
        return 2
    digest = __import__("hashlib").sha256((profile / "swkotor.exe").read_bytes()).hexdigest()
    if digest not in (PATCHED_SHA, BUILD_SHA):
        print(f"profile exe hash {digest} is neither the build nor the patched windowed copy", file=sys.stderr)
        return 2
    lane = Path(args.label)
    run = LANE / "runs" / lane.name
    if run.exists():
        print(f"run exists: {run}", file=sys.stderr)
        return 2
    # The harness creates the run directory itself and refuses a pre-existing one; only the parent is ours.
    (LANE / "runs").mkdir(parents=True, exist_ok=True)
    log_path = LANE / (lane.name + ".log")
    log = log_path.open("w", encoding="utf-8", newline="\n")

    def say(message: str) -> None:
        line = f"[{time.strftime('%H:%M:%S')}] {message}"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    say(f"label={lane.name} profile={profile} exe_sha={digest} elevated={_elevated()}")

    sys.path.insert(0, str(REPO / "tools" / "hydralysis" / "src"))
    from hydralysis import api, trace

    env = os.environ.copy()
    env["SP1_HOOK"] = str(Path(args.hook))
    env["PYTHONPATH"] = os.pathsep.join([str(REPO / "tools" / "hydra-root" / "runtime-backend" / "frida-17.24.3"),
                                         str(REPO / "tools" / "hydralysis" / "src"), env.get("PYTHONPATH", "")])
    cmd = [sys.executable, str(HARNESS), "--label", lane.name, "--scenario", str(Path(args.scenario)),
           "--consent", "--runs-dir", str(LANE / "runs")]
    say("launching harness: " + " ".join(cmd))
    harness = subprocess.Popen(cmd, cwd=str(REPO), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    pid = None
    for _ in range(120):
        launch = _json(run / "launch.json") or {}
        if launch.get("pid"):
            pid = int(launch["pid"])
            break
        if harness.poll() is not None:
            break
        time.sleep(0.5)
    if not pid:
        output = harness.stdout.read() if harness.stdout else ""
        say("harness produced no game pid; output:\n" + output[-3000:])
        return 3
    say(f"harness={harness.pid} game={pid} resumed={_json(run / 'launch.json', {}).get('resumed')}")

    if args.no_trace:
        try:
            output, _ = harness.communicate(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            harness.kill()
            output = "harness timeout"
        say(f"harness exit={harness.returncode}\n{(output or '')[-6000:]}")
        log.close()
        return 0 if harness.returncode == 0 else 5

    settings = api._settings()
    client = trace.TraceClient(trace._engine(settings))
    options = {
        "profile": str(Path(args.trace_profile)),
        "images": {"swkotor.exe": str(profile / "swkotor.exe")},
        "project": str(REPO / "artifacts" / "ghidra" / "kotor.gpr"),
        "program": "/swkotor.exe",
        "scenario": lane.name,
        "launch": {"action": "attach", "exe": str(profile / "swkotor.exe"), "sha256": digest, "pid": pid},
        "capture_options": {"rings": 16, "ring_bytes": 1048576, "batch_ms": 10},
    }
    session = None
    timeline = (run / "trace-timeline.jsonl").open("w", encoding="utf-8", newline="\n")
    summary: dict = {"label": lane.name, "pid": pid, "profile_sha256": digest}
    try:
        started = trace.handle_action("start", None, options, settings, client=client)
        session = started.get("session")
        summary["trace_session"] = session
        summary["profile"] = started.get("profile")
        probes = ((started.get("runtime") or {}).get("capture") or {}).get("trace", {}).get("probes") or []
        summary["probes"] = [{"name": p.get("name"), "hook": p.get("hook"), "address": p.get("address")} for p in probes]
        say(f"trace={session} probes={[p.get('name') for p in probes]}")

        deadline = time.time() + args.timeout
        stopped_early = None
        first_exec = None
        while harness.poll() is None and time.time() < deadline:
            time.sleep(args.poll)
            status = trace.handle_action("status", session, {}, settings, client=client)
            row = {"t": round(time.time(), 3), "events": status.get("events"),
                   "sources": [(s.get("source_id"), s.get("records")) for s in status.get("sources") or []],
                   "gaps": len((status.get("coverage") or {}).get("gaps") or [])}
            timeline.write(json.dumps(row) + "\n")
            timeline.flush()
            say(f"status {row}")
            if stopped_early is None and not args.no_early_stop:
                seen = trace.handle_action("query", session, {"kinds": ["kotor.exec"], "limit": 1},
                                           settings, client=client)
                if (seen.get("rows") or []) and first_exec is None:
                    first_exec = time.time()
                    say("first kotor.exec seen; will stop after the grace period while the game is alive")
                if first_exec is not None and time.time() - first_exec >= args.grace:
                    stopped_early = trace.handle_action("stop", session, {}, settings, client=client)
                    summary["early_stop"] = stopped_early
                    say(f"early stop pump={stopped_early.get('pump')} closed={bool(stopped_early.get('closed'))} "
                        f"errors={stopped_early.get('errors')}")
        try:
            output, _ = harness.communicate(timeout=max(10, deadline - time.time() + 30))
        except subprocess.TimeoutExpired:
            harness.kill()
            output = "harness timeout"
        summary["harness_exit"] = harness.returncode
        say(f"harness exit={harness.returncode}\n{(output or '')[-4000:]}")

        status = trace.handle_action("status", session, {}, settings, client=client)
        summary["events"] = status.get("events")
        summary["sources"] = [(s.get("source_id"), s.get("records")) for s in status.get("sources") or []]
        summary["coverage_gaps"] = len((status.get("coverage") or {}).get("gaps") or [])
        query = trace.handle_action("query", session, {"limit": 200}, settings, client=client)
        rows = query.get("rows") or []
        summary["query_rows"] = len(rows)
        summary["kinds"] = _kinds(rows)
        (run / "query.json").write_text(json.dumps(query, indent=2, default=str) + "\n", encoding="utf-8")
        say(f"query rows={len(rows)} kinds={summary['kinds']}")
        if rows:
            say("first rows:\n" + json.dumps(rows[:10], indent=2, default=str)[:6000])
    finally:
        if session and stopped_early is None:
            try:
                stopped = trace.handle_action("stop", session, {}, settings, client=client)
                summary["pump"] = stopped.get("pump")
                summary["closed"] = bool(stopped.get("closed"))
                summary["errors"] = stopped.get("errors")
                say(f"stop pump={summary.get('pump')} closed={summary.get('closed')} errors={summary.get('errors')}")
            except Exception as exc:  # noqa: BLE001 - report, never mask the capture
                summary["stop_error"] = f"{type(exc).__name__}: {exc}"
                say("stop failed: " + summary["stop_error"])
        timeline.close()
        (run / "trace-summary.json").write_text(json.dumps(summary, indent=2, default=str) + "\n", encoding="utf-8")
        say("summary " + json.dumps(summary, default=str)[:4000])
        log.close()
    return 0 if summary.get("events") else 4


def _elevated() -> bool:
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:  # noqa: BLE001
        return False


if __name__ == "__main__":
    raise SystemExit(main())
