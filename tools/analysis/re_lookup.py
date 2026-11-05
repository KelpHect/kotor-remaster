#!/usr/bin/env python3
"""Print recovered semantics for a script action or function of the user-owned binary.

Reads only the git-ignored local exports under artifacts/; nothing it prints may be
committed (see tools/README.md). Use the output to cite behaviour, not to copy code.

    re_lookup.py action SetLocked          # handler pseudocode by action name
    re_lookup.py action 509                # ... or by action id
    re_lookup.py fn 00544390               # pseudocode by entry address
    re_lookup.py find StartNewModule       # names matching a regex
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "artifacts" / "action_table.json"
INVENTORY = ROOT / "artifacts" / "lanes" / "engine-semantics" / "export-v4" / "functions-v4.jsonl"
SOURCES = (
    ROOT / "artifacts" / "ghidra" / "functions-full.jsonl",
    ROOT / "artifacts" / "analysis" / "ghidra-handlers" / "new_functions.jsonl",
    ROOT / "artifacts" / "analysis" / "ghidra-handlers" / "copy_functions.jsonl",
)


def rows(path: Path):
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            yield json.loads(line)


def names() -> dict[str, str]:
    return {row["entry"].lower(): row["name"] for row in rows(INVENTORY)}


def pseudocode(entry: str) -> str | None:
    entry = entry.lower().removeprefix("fun_")
    for source in SOURCES:
        if not source.exists():
            continue
        for row in rows(source):
            if row.get("entry", "").lower() == entry and row.get("pseudocode"):
                return row["pseudocode"]
    return None


def resolve_action(key: str) -> dict:
    for action in json.loads(TABLE.read_text(encoding="utf-8"))["actions"]:
        if str(action.get("id")) == key or str(action.get("name", "")).lower() == key.lower():
            return action
    raise SystemExit(f"no such action: {key}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("action", "fn", "find"))
    parser.add_argument("key")
    args = parser.parse_args()
    labels = names()
    if args.mode == "find":
        pattern = re.compile(args.key, re.IGNORECASE)
        for entry, name in labels.items():
            if pattern.search(name):
                print(entry, name)
        return 0
    if args.mode == "action":
        action = resolve_action(args.key)
        print(f"// action {action['id']} {action['name']}({action.get('args', '')})")
        entry = str(action.get("handler", "")).lower().removeprefix("fun_")
    else:
        entry = args.key.lower().removeprefix("fun_")
    print(f"// entry {entry} name {labels.get(entry, '?')}")
    code = pseudocode(entry)
    if code is None:
        print("// no pseudocode in the local exports", file=sys.stderr)
        return 1
    print(re.sub(r"FUN_([0-9a-fA-F]{8})",
                 lambda m: labels.get(m.group(1).lower(), m.group(0)), code))
    return 0


if __name__ == "__main__":
    sys.exit(main())
