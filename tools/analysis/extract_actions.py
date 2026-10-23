#!/usr/bin/env python3
"""Build the KotOR 1 action table from two local sources:

1. The shipped `nwscript.nss` declaration order (extracted read-only from the user's BIF).
2. The decompiled `InitializeCommands` function (entry 0x0054c960) in functions-full.jsonl.

Writes an extracted copy of nwscript.nss and a JSON mapping action id -> name -> handler.
The extracted source is game data: keep it under artifacts/ (gitignored), never commit it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "inventory"))
from kotor_inventory import parse_bif, parse_key  # noqa: E402

INITIALIZER = "0054c960"
ACTION_COUNT = 772


def bif_path(game_dir: str, key: dict, bif_index: int) -> str:
    meta = key["bifs"][bif_index]
    name = meta["name"].replace("\\", "/")
    if name.lower().startswith("data/"):
        return os.path.join(game_dir, name)
    return os.path.join(game_dir, "data", os.path.basename(name))


def read_key_resource(game_dir: str, key: dict, resref: str, rtype: int) -> tuple[bytes, dict]:
    matches = [e for e in key["entries"] if e["resref"].lower() == resref.lower() and e["type"] == rtype]
    if not matches:
        raise SystemExit(f"resource {resref!r} type {rtype} not found in chitin.key")
    entry = matches[0]
    path = bif_path(game_dir, key, entry["bif"])
    bif = parse_bif(path)
    rows = bif["var"] + bif["fixed"]
    row = rows[entry["index"]]
    with open(path, "rb") as f:
        f.seek(row["offset"])
        data = f.read(row["size"])
    return data, {"entry": entry, "bif": path, "row": row}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


TYPE_ALT = r"(?:void|int|float|string|object|vector|location|effect|event|talent|action|itemproperty)"
PROTO = re.compile(rf"\b{TYPE_ALT}\s+([A-Za-z_]\w*)\s*\(([^;]*?)\)\s*;", re.S)
TABLE_BASE = "param_1 + 0xc"
HANDLER_RE = re.compile(r"^[A-Za-z_]\w*$")
OFFSET_RE = re.compile(re.escape(TABLE_BASE) + r"\) \+ (0x[0-9a-fA-F]+|\d+)\)")
SLOT0_RE = re.compile(r"\*{2}\([^)]*\)\(" + re.escape(TABLE_BASE) + r"\)")


def parse_table_writes(pseudocode: str) -> list[tuple[int, str, str]]:
    """Return (offset, handler, source_line).

    Ghidra prints some offsets in decimal and slot 0 without an explicit offset;
    both forms must be accepted. Only assignments whose right-hand side is a
    single code symbol are considered.
    """
    writes: list[tuple[int, str, str]] = []
    for line in pseudocode.splitlines():
        if TABLE_BASE not in line or "=" not in line:
            continue
        lhs, rhs = line.rsplit("=", 1)
        if TABLE_BASE not in lhs:
            continue
        target = rhs.strip().rstrip(";").strip()
        if target.startswith("&"):
            target = target[1:]
        if not HANDLER_RE.fullmatch(target):
            continue
        match = OFFSET_RE.search(lhs)
        if match:
            offset = int(match.group(1), 0)
        elif SLOT0_RE.search(lhs):
            offset = 0
        else:
            continue
        writes.append((offset, target, line.strip()))
    return writes


def parse_prototypes(text: str) -> list[dict]:
    clean = strip_comments(text)
    out = []
    for m in PROTO.finditer(clean):
        args = " ".join(m.group(2).split())
        out.append({"name": m.group(1), "args": args})
    return out


def scan_handlers(rows: list[dict]) -> tuple[dict[int, dict], list[dict]]:
    """Scan every decompiled function for writes into the 772-slot action table.

    The primary initializer (0x0054c960) is followed by a secondary registration
    function (0x005cd2c0) called at its end. Other functions with the same write
    pattern are reported so they can be reviewed rather than trusted blindly.
    """
    handlers: dict[int, dict] = {}
    sources: list[dict] = []
    total_writes = 0
    for row in rows:
        writes = parse_table_writes(row.get("pseudocode", ""))
        if not writes:
            continue
        count = 0
        for offset, target, _line in writes:
            index = offset // 4
            if 0 <= index < ACTION_COUNT:
                handler = {"handler": target, "source": row["entry"]}
                if index in handlers and handlers[index]["handler"] != target:
                    handler["overwritten"] = True
                handlers[index] = handler
                count += 1
                total_writes += 1
        sources.append({"entry": row["entry"], "assignments": count})
    sources.sort(key=lambda x: -x["assignments"])
    return handlers, sources, total_writes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game-dir", required=True)
    ap.add_argument("--functions", default="artifacts/ghidra/functions-full.jsonl")
    ap.add_argument("--nwscript-out", default="artifacts/game-extract/nwscript.nss")
    ap.add_argument("--out", default="artifacts/action_table.json")
    args = ap.parse_args()

    key = parse_key(os.path.join(args.game_dir, "chitin.key"))
    nss, provenance = read_key_resource(args.game_dir, key, "nwscript", 2009)
    os.makedirs(os.path.dirname(os.path.abspath(args.nwscript_out)), exist_ok=True)
    with open(args.nwscript_out, "wb") as f:
        f.write(nss)
    text = nss.decode("cp1252", "replace")
    protos = parse_prototypes(text)

    target = None
    with open(args.functions, encoding="utf-8") as f:
        for line in f:
            if not line.strip():
                continue
            row = json.loads(line)
            if row["entry"] == INITIALIZER:
                target = row
                break
    if not target or "pseudocode" not in target:
        raise SystemExit(f"initializer {INITIALIZER} not found/decompiled in {args.functions}")

    rows = [json.loads(line) for line in open(args.functions, encoding="utf-8") if line.strip()]
    handlers, sources, total_writes = scan_handlers(rows)
    actions = []
    for i in range(ACTION_COUNT):
        reg = handlers.get(i, {})
        actions.append({
            "id": i,
            "name": protos[i]["name"] if i < len(protos) else None,
            "args": protos[i]["args"] if i < len(protos) else None,
            "handler": reg.get("handler"),
            "registered_by": reg.get("source"),
        })
    missing = [a for a in actions if a["handler"] is None]
    report = {
        "build_sha256": hashlib.sha256(open(os.path.join(args.game_dir, "swkotor.exe"), "rb").read()).hexdigest(),
        "initializer": INITIALIZER,
        "initializer_size": target.get("size"),
        "initializer_pseudocode_sha256": target.get("pseudocode_sha256"),
        "registration_sources": sources,
        "dispatch": {
            "entry": "0052c0d0",
            "table_member_offset": "0xc",
            "bound": ACTION_COUNT,
            "null_result": "0xfffff82e",
            "vtable": "0x7450e4",
            "vtable_init_slot": "+4",
            "vtable_dispatch_slot": "+8",
            "note": "dispatcher bytes verified at VA 0x0052c0d0; vtable pointers 0x0054c960 at 0x7450e8 and 0x0052c0d0 at 0x7450ec verified in the file",
        },
        "nwscript": {"resref": "nwscript", "type": 2009, "bif": provenance["bif"],
                     "offset": provenance["row"]["offset"], "size": provenance["row"]["size"]},
        "counts": {
            "declared_prototypes": len(protos),
            "action_slots": ACTION_COUNT,
            "total_table_writes": total_writes,
            "duplicate_writes": total_writes - len(handlers),
            "assigned_handlers": len(handlers),
            "unassigned_slots": len(missing),
        },
        "unassigned": [{"id": a["id"], "name": a["name"]} for a in missing],
        "actions": actions,
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)

    print(f"nwscript.nss: {len(nss)} bytes, {len(protos)} prototypes -> {args.nwscript_out}")
    print(f"registration sources: {sources} (total writes {total_writes})")
    print(f"action table: {len(handlers)} assigned slots, {len(missing)} unassigned")
    for i in (0, 1, 2, 3, 25, 32, 50, 75, 77, 100, 125, 250, 500, 520, 582, 660, 700, 771):
        if i < len(actions):
            a = actions[i]
            print(f"  {i:>3}: {str(a['name']):<32} {a['handler']}")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
