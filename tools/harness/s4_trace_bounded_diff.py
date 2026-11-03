#!/usr/bin/env python3
"""Bounded semantic diff of two KotOR traces: import the common script-action prefix into two fresh trace
sessions and ask the engine's aligned diff whether every action group matches.

    python tools/harness/s4_trace_bounded_diff.py \
        --a artifacts/lanes/s4-trace/live-04-semantic.jsonl \
        --b artifacts/lanes/s4-trace/live-05-semantic.jsonl \
        --out artifacts/lanes/s4-trace/diff-bounded.json

The prefix is the longest run of identical `kotor.action` ids from the start (optionally capped by --limit).
Only those events are imported, with fresh sequential source sequences, so producer gaps cannot make the diff
`not_comparable`; the aligned diff groups by `kind` + `payload:action` and reports `same` only when the counts
and the key agree.
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def actions(path: Path) -> list[dict]:
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        event = json.loads(line)
        if event.get("kind") == "kotor.action":
            payload = event.get("payload")
            rows.append({**event, "payload": json.loads(payload) if isinstance(payload, str) else (payload or {})})
    return rows


def common_prefix(a: list[dict], b: list[dict], limit: int = 0) -> int:
    n = 0
    for left, right in zip(a, b):
        if left["payload"].get("action") != right["payload"].get("action"):
            break
        n += 1
        if limit and n >= limit:
            break
    return n


def import_prefix(client, events: list[dict], label: str) -> str:
    opened = client.begin(create=True, mode="write", meta={"scenario": label, "limitations": []})
    session, cap = opened["session"], opened["lease"]["cap"]
    batch = []
    for sequence, event in enumerate(events, 1):
        row = {key: event[key] for key in ("kind", "t_mono_ns", "process_id", "thread_id", "payload") if key in event}
        row.update({"source_id": "agent", "source_seq": sequence, "producer_gen": 1, "clock_domain": "qpc-10mhz",
                    "acquisition": "atomic", "confidence": "exact"})
        batch.append(row)
    reply = client.append(session, batch, cap=cap)
    if reply.get("rejected"):
        raise SystemExit("append rejected: %s" % reply["rejected"][:3])
    client.close(session, cap=cap)
    return session


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--a", required=True, help="JSONL from s4_trace_extract.py (first session)")
    ap.add_argument("--b", required=True, help="JSONL from s4_trace_extract.py (second session)")
    ap.add_argument("--limit", type=int, default=0, help="cap the common prefix (0: the whole common prefix)")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    left, right = actions(Path(args.a)), actions(Path(args.b))
    n = common_prefix(left, right, args.limit)
    if n < 3:
        print(json.dumps({"ok": False, "reason": "common action prefix is shorter than 3", "prefix": n}, indent=2))
        return 2

    sys.path.insert(0, str(REPO / "tools" / "hydralysis" / "src"))
    from hydralysis import api, trace
    settings = api._settings()
    client = trace.TraceClient(trace._engine(settings))
    session_a = import_prefix(client, left[:n], "kotor-prefix-a")
    session_b = import_prefix(client, right[:n], "kotor-prefix-b")
    result = client.diff(session_a, session_b, kinds=["kotor.action"], align={"keys": ["kind", "payload:action"]})
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"sessions": {"a": session_a, "b": session_b}, "prefix": n, **result},
                              indent=2) + "\n", encoding="utf-8")
    aligned = result.get("aligned") or {}
    classes = Counter(entry["class"] for entry in aligned.get("entries") or [])
    print(json.dumps({"sessions": {"a": session_a, "b": session_b}, "prefix": n,
                      "entries": len(aligned.get("entries") or []), "classes": dict(classes),
                      "scanned": [aligned.get("scanned_a"), aligned.get("scanned_b")],
                      "unmatched": [aligned.get("unmatched_a"), aligned.get("unmatched_b")],
                      "gaps": [aligned.get("gaps_a"), aligned.get("gaps_b")], "out": str(out)}, indent=2))
    return 0 if classes and set(classes) == {"same"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
