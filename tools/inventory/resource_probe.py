#!/usr/bin/env python3
"""Probe resource payload signatures per KotOR resource type.

Resolves KEY entries to BIF table entries and prints the first bytes of a few
resources for each type, plus one example per type found inside RIM modules.
This empirically identifies type IDs (NCS, GFF, TPC, WAV, MDL/MDX, ...).
Read-only.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from collections import defaultdict

from kotor_inventory import parse_bif, parse_key, parse_rim


def ascii_preview(data: bytes, width: int = 24) -> str:
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data[:width])


def resolve(entry: dict, bifs: dict[int, dict]) -> tuple[dict, bytes] | None:
    bif = bifs.get(entry["bif"])
    if not bif:
        return None
    rows = bif["var"] + bif["fixed"]
    idx = entry["index"]
    if idx >= len(rows):
        return None
    row = rows[idx]
    path = bif["path"]
    with open(path, "rb") as f:
        f.seek(row["offset"])
        data = f.read(min(row["size"], 64))
    return row, data


def probe_key(game_dir: str, per_type: int = 2) -> None:
    key_path = os.path.join(game_dir, "chitin.key")
    key = parse_key(key_path)
    bifs: dict[int, dict] = {}
    for meta in key["bifs"]:
        name = meta["name"].replace("\\", "/")
        rel = name if name.lower().startswith("data/") else os.path.join("data", os.path.basename(name))
        path = os.path.join(game_dir, rel)
        if os.path.exists(path):
            b = parse_bif(path)
            b["path"] = path
            bifs[meta["index"]] = b

    by_type: dict[int, list[dict]] = defaultdict(list)
    for e in key["entries"]:
        by_type[e["type"]].append(e)

    print("KEY/BIF resource signatures (first bytes of payload):")
    for rtype in sorted(by_type):
        shown = 0
        for e in by_type[rtype]:
            res = resolve(e, bifs)
            if not res:
                continue
            row, data = res
            print(f"  type {rtype:>5}  {e['resref']:<18} bif={e['bif']:<3} idx={e['index']:<7} "
                  f"size={row['size']:<8} {data.hex()[:32]}  {ascii_preview(data)}")
            shown += 1
            if shown >= per_type:
                break


def probe_modules(game_dir: str) -> None:
    mod_dir = os.path.join(game_dir, "modules")
    seen: dict[int, list[str]] = defaultdict(list)
    for fn in sorted(os.listdir(mod_dir)):
        if not fn.lower().endswith(".rim"):
            continue
        path = os.path.join(mod_dir, fn)
        rim = parse_rim(path)
        if not rim:
            continue
        with open(path, "rb") as f:
            for e in rim["entries"]:
                if len(seen[e["type"]]) >= 2:
                    continue
                f.seek(e["offset"])
                data = f.read(min(e["size"], 32))
                seen[e["type"]].append(
                    f"type {e['type']:>5}  {fn:<20} {e['resref']:<18} size={e['size']:<7} "
                    f"{data.hex()[:32]}  {ascii_preview(data)}"
                )
    print("\nModule RIM resource signatures (first example per type):")
    for rtype in sorted(seen):
        for line in seen[rtype][:1]:
            print("  " + line)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game-dir", required=True)
    ap.add_argument("--per-type", type=int, default=2)
    ap.add_argument("--skip-modules", action="store_true")
    args = ap.parse_args()
    probe_key(args.game_dir, args.per_type)
    if not args.skip_modules:
        probe_modules(args.game_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
