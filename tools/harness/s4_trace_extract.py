#!/usr/bin/env python3
"""Extract every semantic event of a trace session to JSONL (payload decoded), for a KotOR report or a diff.

    python tools/harness/s4_trace_extract.py --session tr_... --out artifacts/lanes/s4-trace/live.jsonl
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_KINDS = ("kotor.exec", "kotor.action", "kotor.command", "kotor.frame")


def payload(row: dict) -> dict:
    value = row.get("payload")
    return json.loads(value) if isinstance(value, str) else (value or {})


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--session", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--kinds", default=",".join(DEFAULT_KINDS))
    ap.add_argument("--limit", type=int, default=1000, help="rows per engine page (the engine caps at 1000)")
    args = ap.parse_args()
    kinds = [item.strip() for item in args.kinds.split(",") if item.strip()]

    sys.path.insert(0, str(REPO / "tools" / "hydralysis" / "src"))
    from hydralysis import api, trace

    settings = api._settings()
    client = trace.TraceClient(trace._engine(settings))
    rows, cursor, pages = [], None, 0
    while True:
        reply = client.query(args.session, kinds=kinds, limit=args.limit, after=cursor)
        batch = reply.get("rows") or []
        rows.extend(batch)
        pages += 1
        cursor = reply.get("next")
        if not cursor or not batch:
            break
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8", newline="\n") as handle:
        for row in rows:
            handle.write(json.dumps({"kind": row["kind"], "seq": row.get("seq"), "t_mono_ns": row.get("t_mono_ns"),
                                     "thread_id": row.get("thread_id"), "payload": payload(row),
                                     "callstack": row.get("callstack")}, sort_keys=True) + "\n")
    counts = Counter(row["kind"] for row in rows)
    actions = Counter(payload(row).get("action") for row in rows if row["kind"] == "kotor.action")
    print(json.dumps({"session": args.session, "pages": pages, "rows": len(rows), "kinds": dict(counts),
                      "distinct_actions": len(actions), "top_actions": actions.most_common(12),
                      "out": str(out)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
