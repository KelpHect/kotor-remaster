#!/usr/bin/env python3
"""Build the 772-action specification surface for the KotOR 1.0.3.0 script VM.

Inputs are read-only data artifacts:
  * ``artifacts/action_table.json`` - the verified 772-slot action table
    (id/name/args/handler/registration).
  * ``artifacts/game-extract/nwscript.nss`` - the shipped prototype source
    (extracted read-only from ``data/scripts.bif``).
  * ``artifacts/lanes/engine-semantics/export-v4/functions-v4.jsonl`` - the
    v4 function inventory used to confirm every handler address.

The output is a machine-readable spec (``action_specs.json``) with, for every
action: id, name, return type, parameter list, required/total arity, engine
handler address, handler presence, and the typed default stub the independent
VM uses.  It is metadata, not game content or decompiled program text.

Usage:
    python tools/analysis/action_specs.py \
        --actions artifacts/action_table.json \
        --nwscript artifacts/game-extract/nwscript.nss \
        --functions artifacts/lanes/engine-semantics/export-v4/functions-v4.jsonl \
        --out artifacts/lanes/s2-01-engine-map/action_specs.json
"""
from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from pathlib import Path

ACTION_COUNT = 772
PROTO_RE = re.compile(
    r"^\s*([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\(([^;]*)\)\s*;", re.MULTILINE
)
HANDLER_RE = re.compile(r"([0-9A-Fa-f]{8})")

DEFAULTS = {
    "int": 0,
    "float": 0.0,
    "string": "",
    "object": 0x7F000000,
    "vector": [0.0, 0.0, 0.0],
    "effect": 0,
    "event": 0,
    "location": 0,
    "talent": 0,
    "void": None,
}


def split_params(text: str) -> list[str]:
    """Split a C-style parameter list on top-level commas."""
    out: list[str] = []
    cur: list[str] = []
    depth = 0
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            item = "".join(cur).strip()
            if item:
                out.append(item)
            cur = []
        else:
            cur.append(ch)
    item = "".join(cur).strip()
    if item:
        out.append(item)
    return out


def parse_prototypes(text: str) -> list[dict]:
    """Return {ret, name, args, params} in declaration order."""
    out = []
    for m in PROTO_RE.finditer(text):
        ret, name, args = m.group(1), m.group(2), " ".join(m.group(3).split())
        params = []
        for decl in split_params(args):
            head = decl.split("=", 1)[0].strip()
            bits = head.split(None, 1)
            ptype = bits[0] if bits else ""
            pname = bits[1].strip() if len(bits) > 1 else ""
            params.append({
                "decl": decl,
                "type": ptype,
                "name": pname,
                "optional": "=" in decl,
            })
        out.append({"ret": ret, "name": name, "args": args, "params": params})
    return out


def handler_address(name: str) -> str | None:
    m = HANDLER_RE.search(name or "")
    return m.group(1).lower() if m else None


def stub_text(ret: str) -> str:
    if ret == "void":
        return "return"
    if ret == "vector":
        return "return vector(0.0, 0.0, 0.0)"
    if ret == "string":
        return 'return ""'
    if ret == "object":
        return "return OBJECT_INVALID"
    return "return 0"


def build_specs(actions: dict, nwscript_text: str, functions_path: Path) -> dict:
    protos = parse_prototypes(nwscript_text)
    rows = list(actions["actions"])
    if len(rows) != ACTION_COUNT:
        raise ValueError(f"action table has {len(rows)} rows, expected {ACTION_COUNT}")
    if len(protos) != ACTION_COUNT:
        raise ValueError(f"nwscript has {len(protos)} prototypes, expected {ACTION_COUNT}")

    entries: set[str] = set()
    role_by_entry: dict[str, str] = {}
    with functions_path.open(encoding="utf-8") as f:
        for line in f:
            if not line.strip():
                continue
            row = json.loads(line)
            entry = row["entry"].lower()
            entries.add(entry)
            role_by_entry[entry] = row.get("role") or ""

    specs = []
    for i, (row, proto) in enumerate(zip(rows, protos)):
        if row["id"] != i:
            raise ValueError(f"action row {i} has id {row['id']}")
        if row["name"] != proto["name"]:
            raise ValueError(f"action {i}: table {row['name']!r} != nwscript {proto['name']!r}")
        ret = proto["ret"]
        if ret not in DEFAULTS:
            raise ValueError(f"action {i} ({row['name']}): unknown return type {ret!r}")
        addr = handler_address(row.get("handler", ""))
        params = proto["params"]
        specs.append({
            "id": i,
            "name": row["name"],
            "return": ret,
            "args": proto["args"],
            "params": params,
            "required": sum(1 for p in params if not p["optional"]),
            "total": len(params),
            "handler": row.get("handler"),
            "handler_address": f"0x{addr}" if addr else None,
            "handler_present": bool(addr and addr in entries),
            "registered_by": row.get("registered_by"),
            "default": DEFAULTS[ret],
            "stub": stub_text(ret),
        })

    missing_handlers = [s["id"] for s in specs if not s["handler_present"]]
    return {
        "build_sha256": actions.get("build_sha256"),
        "count": len(specs),
        "return_types": dict(sorted(Counter(s["return"] for s in specs).items())),
        "handlers_unique": len({s["handler_address"] for s in specs if s["handler_address"]}),
        "handler_functions_present": len(specs) - len(missing_handlers),
        "missing_handler_ids": missing_handlers,
        "actions": specs,
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--actions", required=True)
    ap.add_argument("--nwscript", required=True)
    ap.add_argument("--functions", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    actions = json.loads(Path(args.actions).read_text(encoding="utf-8"))
    text = Path(args.nwscript).read_text(encoding="latin-1")
    spec = build_specs(actions, text, Path(args.functions))
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(spec, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({k: spec[k] for k in (
        "count", "return_types", "handlers_unique", "handler_functions_present",
        "missing_handler_ids")}, sort_keys=True))
    return 0 if not spec["missing_handler_ids"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
