#!/usr/bin/env python3
"""Split one multi-module live events.jsonl into per-module event files.

A completed transition opens `currentgame\\<module>.rim`; each such open starts
a new segment. Events before the first segment are ignored. Repeated visits to
the same module are appended to that module's segment.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


CURRENT_RE = re.compile(r"currentgame[\\/]([A-Za-z0-9_]+)\.(?:rim|sav|mod|nwm)", re.I)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--events", required=True)
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args(argv)

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    segments: dict[str, list[dict]] = {}
    current = None
    with Path(args.events).open(encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            event = json.loads(line)
            path = event.get("path", "")
            match = CURRENT_RE.search(path) if path else None
            if match:
                current = match.group(1).lower()
                segments.setdefault(current, [])
            if current is not None:
                segments[current].append(event)
    for module, rows in sorted(segments.items()):
        path = out / f"events-{module}.jsonl"
        with path.open("w", encoding="utf-8", newline="\n") as handle:
            for row in rows:
                handle.write(json.dumps(row, sort_keys=True) + "\n")
    print(json.dumps({"modules": len(segments),
                      "names": sorted(segments)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
