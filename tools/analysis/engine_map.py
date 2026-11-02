#!/usr/bin/env python3
"""Build the P2 engine map and evaluate gate G2.

Reads the v4 function inventory, the verified 772-entry action table, and the
action specification JSON produced by ``tools/analysis/action_specs.py``.
Writes a machine-readable ``engine_map.json`` and a tracked Markdown summary
(``docs/recon/26_ENGINE_MAP.md``).

The function inventory is the v4 export for build SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
No game assets or decompiled program text are written.

Usage:
    python tools/analysis/engine_map.py \
        --functions artifacts/lanes/engine-semantics/export-v4/functions-v4.jsonl \
        --actions artifacts/action_table.json \
        --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
        --out-json artifacts/lanes/s2-01-engine-map/engine_map.json \
        --out-doc docs/recon/26_ENGINE_MAP.md
"""
from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path

BUILD = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6"

# Representative, evidence-cited functions per subsystem.  The full function
# inventory is classified separately; this table is the interface map.
SUBSYSTEMS = [
    {
        "name": "script-vm",
        "owner": "script service",
        "evidence": "docs/recon/02 §3; docs/recon/14",
        "functions": [
            ("0x0054c960", "action-table initializer (677 slots, tail-jumps to 0x005cd2c0)"),
            ("0x005cd2c0", "action-table tail (97 slots; 774 writes, ids 156/709 duplicated)"),
            ("0x0052c0d0", "action dispatcher (vtable 0x7450e4 +8, bound 772, null -> -2002)"),
        ],
    },
    {
        "name": "resource-vfs",
        "owner": "resource manager / module loader",
        "evidence": "docs/recon/23 #23-#25; docs/recon/21",
        "functions": [
            ("0x004c44d0", "five-pass module/area mounter"),
            ("0x004babb0", "game-loop module load/transition path"),
            ("0x004c5720", "area module mount"),
        ],
    },
    {
        "name": "save-gff",
        "owner": "save system",
        "evidence": "docs/recon/21; docs/recon/23 #23-#25",
        "functions": [
            ("0x00411730", "GFF field-index append allocator"),
            ("0x00411020", "GFF fidx growth/append"),
            ("0x004113d0", "GFF six-section writer"),
        ],
    },
    {
        "name": "renderer-gl",
        "owner": "renderer",
        "evidence": "docs/recon/22; docs/recon/23 §D",
        "functions": [
            ("0x0046f900", "mesh/indexed draw path"),
            ("0x00425ab0", "glDrawElements wrapper"),
            ("0x00420f20", "save/screenshot TGA path"),
        ],
    },
    {
        "name": "input-camera",
        "owner": "input / camera",
        "evidence": "docs/recon/12; docs/recon/22; docs/recon/25",
        "functions": [
            ("0x006227e0", "per-frame input update (ECX = input object)"),
            ("0x00621210", "game action dispatcher (Quick Save = 0xDA)"),
            ("0x006bb6f8", "camera +0x38 write"),
            ("0x0062fed0", "dialogue camera controller ctor"),
        ],
    },
    {
        "name": "audio-stream",
        "owner": "audio",
        "evidence": "docs/recon/23 §D",
        "functions": [
            ("0x005dca30", "voice-stream request builder (HD0:STREAMWAVES)"),
            ("0x005dbd30", "AIL_open_stream wrapper"),
        ],
    },
    {
        "name": "chargen-ui",
        "owner": "chargen / UI",
        "evidence": "docs/recon/22",
        "functions": [
            ("0x006eb420", "chargen manager + sub-screen ctor"),
            ("0x006eb320", "real PLAY/start-module handler"),
        ],
    },
    {
        "name": "minimap",
        "owner": "GUI / area map",
        "evidence": "docs/recon/19 §6",
        "functions": [
            ("0x00508c50", "ARE Map loader; reads MapResX/NorthAxis/MapZoom/MapPt/WorldPt"),
            ("0x00578c60", "minimap affine scale/offset setup"),
        ],
    },
]


def load_functions(path: Path) -> list[dict]:
    rows = []
    with path.open(encoding="utf-8") as f:
        for line in f:
            if line.strip():
                rows.append(json.loads(line))
    return rows


def build_map(functions: list[dict], actions: dict, specs: dict) -> dict:
    categories = Counter(r.get("category") or "unknown" for r in functions)
    roles = Counter(r.get("role") or "unassigned" for r in functions)
    engine = sum(1 for r in functions if r.get("category") == "engine")
    unknown = sum(1 for r in functions if (r.get("category") or "unknown") == "unknown")
    handlers = [s for s in specs["actions"] if s.get("handler_address")]
    unique_handlers = {s["handler_address"] for s in handlers}
    missing_specs = [s["id"] for s in specs["actions"] if not s.get("stub")]
    missing_handlers = [s["id"] for s in specs["actions"] if not s.get("handler_present")]
    dispatch = actions.get("dispatch", {})
    return {
        "schema": "kotor-s2-engine-map/1",
        "build_sha256": BUILD,
        "functions": {
            "rows": len(functions),
            "engine": engine,
            "unknown": unknown,
            "categories": dict(sorted(categories.items())),
            "roles": dict(sorted(roles.items())),
            "unexplained_text_bytes": 0,
            "unexplained_source": "docs/recon/14_P0_FUNCTION_CLASSIFICATION.md",
        },
        "actions": {
            "count": actions.get("counts", {}).get("action_slots"),
            "specs": len(specs["actions"]),
            "missing_specs": missing_specs,
            "handlers_unique": len(unique_handlers),
            "handlers_present": len(handlers) - len(missing_handlers),
            "missing_handlers": missing_handlers,
            "return_types": specs.get("return_types", {}),
            "initializer": actions.get("initializer"),
            "initializer_size": actions.get("initializer_size"),
            "dispatch": dispatch,
        },
        "subsystems": SUBSYSTEMS,
        "threading": {
            "main_game_path": "single-threaded in every P1/P2 hook (input, render, chargen, module load, scripts)",
            "audio": "Miles Mss32 owns internal audio threads; no engine-owned worker thread observed",
            "evidence": ["docs/recon/22_DYNAMIC_P1.md", "docs/recon/25_SYNTHETIC_RUNTIME.md"],
        },
        "g2": {
            "engine_functions_classified": engine,
            "unknown": unknown,
            "unexplained_text_bytes": 0,
            "action_specs": len(specs["actions"]),
            "action_handlers_unique": len(unique_handlers),
            "action_handlers_present": len(handlers) - len(missing_handlers),
            "no_unexplained_entry_points": unknown == 0,
            "status": "closed" if (unknown == 0 and not missing_specs and not missing_handlers) else "open",
        },
    }


def _vtable(actions: dict) -> str:
    value = str(actions.get("dispatch", {}).get("vtable", "0x7450e4"))
    return value if value.lower().startswith("0x") else "0x" + value


def markdown(m: dict) -> str:
    f = m["functions"]
    a = m["actions"]
    g = m["g2"]
    lines = [
        "# 26 — P2 engine map and action specification (gate G2)",
        "",
        f"Status: **{g['status']}** — built from the v4 inventory for build SHA-256 "
        f"`{m['build_sha256']}`.",
        "",
        "## G2 checklist",
        "",
        "| item | result | evidence |",
        "|---|---|---|",
        f"| Engine-relevant functions classified | **{f['engine']:,}** | v4 inventory (`functions-v4.jsonl`) |",
        f"| Unknown / medium / low | **{f['unknown']}** | v4 inventory + recon/14 |",
        f"| Unexplained `.text` bytes | **0** | `docs/recon/14_P0_FUNCTION_CLASSIFICATION.md` |",
        f"| Action table slots | **{a['count']}/772** | `artifacts/action_table.json` |",
        f"| Action specs / typed stubs | **{a['specs']}/772** | `action_specs.json` |",
        f"| Unique handler targets | **{a['handlers_unique']}/565** | v4 inventory + action table |",
        f"| Handler functions present | **{a['handlers_present']}/772** | v4 inventory |",
        f"| Dispatcher | **0x{a['dispatch'].get('entry','0052c0d0')}**, bound 772 | vtable `{_vtable(a)}` +8 |",
        f"| No unexplained entry points | **{'yes' if g['no_unexplained_entry_points'] else 'no'}** | v4 inventory |",
        "",
        "## Function inventory",
        "",
        "| category | count |",
        "|---|---:|",
    ]
    for k, v in f["categories"].items():
        lines.append(f"| {k} | {v:,} |")
    lines += [
        f"| **total** | **{f['rows']:,}** |",
        "",
        "Role counts: " + ", ".join(f"`{k}` {v:,}" for k, v in f["roles"].items()) + ".",
        "",
        "## Action table and dispatcher",
        "",
        f"- Initializer `0x{a['initializer']}` ({a['initializer_size']} bytes) writes the 772-slot table; "
        "the tail `0x005cd2c0` supplies the remaining slots (774 writes, ids 156/709 duplicated).",
        f"- Dispatcher `0x{a['dispatch'].get('entry','0052c0d0')}` is vtable `{_vtable(a)}` "
        "slot `+8`; it rejects null handlers with `-2002` and calls `handler(id, arg)`.",
        f"- Return-type mix: " + ", ".join(f"`{k}` {v}" for k, v in a["return_types"].items()) + ".",
        "",
        "## Subsystem map",
        "",
        "| subsystem | owner | representative functions | evidence |",
        "|---|---|---|---|",
    ]
    for s in m["subsystems"]:
        funcs = "; ".join(f"`{addr}` {role}" for addr, role in s["functions"])
        lines.append(f"| {s['name']} | {s['owner']} | {funcs} | {s['evidence']} |")
    t = m["threading"]
    lines += [
        "",
        "## Threads and state ownership",
        "",
        f"- Main game path: {t['main_game_path']}.",
        f"- Audio: {t['audio']}.",
        "- Module/VFS owns mounted archives; the save writer owns GFF/fidx; the script service owns",
        "  the 772-slot action table; the camera controller owns `+0xc4`/`+0x38`; the chargen manager",
        "  owns the `END_M01AA` handoff; input/UI owns the action queue.",
        f"- Evidence: {', '.join('`'+x+'`' for x in t['evidence'])}.",
        "",
        "## Action specifications",
        "",
        "`artifacts/lanes/s2-01-engine-map/action_specs.json` contains all 772 records: id, name,",
        "return type, parameter list, required/total arity, handler address, handler presence,",
        "default value, and stub text. The generator is `tools/analysis/action_specs.py`; it reads",
        "the read-only `action_table.json`, `nwscript.nss`, and v4 inventory, and exits non-zero if",
        "any action lacks a return type, handler, or typed stub.",
        "",
        "## Reproduce",
        "",
        "```bash",
        "python tools/analysis/action_specs.py \\",
        "  --actions artifacts/action_table.json \\",
        "  --nwscript artifacts/game-extract/nwscript.nss \\",
        "  --functions artifacts/lanes/engine-semantics/export-v4/functions-v4.jsonl \\",
        "  --out artifacts/lanes/s2-01-engine-map/action_specs.json",
        "python tools/analysis/engine_map.py \\",
        "  --functions artifacts/lanes/engine-semantics/export-v4/functions-v4.jsonl \\",
        "  --actions artifacts/action_table.json \\",
        "  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \\",
        "  --out-json artifacts/lanes/s2-01-engine-map/engine_map.json \\",
        "  --out-doc docs/recon/26_ENGINE_MAP.md",
        "```",
        "",
    ]
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--functions", required=True)
    ap.add_argument("--actions", required=True)
    ap.add_argument("--specs", required=True)
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--out-doc", required=True)
    args = ap.parse_args(argv)
    functions = load_functions(Path(args.functions))
    actions = json.loads(Path(args.actions).read_text(encoding="utf-8"))
    specs = json.loads(Path(args.specs).read_text(encoding="utf-8"))
    m = build_map(functions, actions, specs)
    out_json = Path(args.out_json)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(m, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    out_doc = Path(args.out_doc)
    out_doc.parent.mkdir(parents=True, exist_ok=True)
    out_doc.write_text(markdown(m) + "\n", encoding="utf-8")
    print(json.dumps(m["g2"], sort_keys=True))
    return 0 if m["g2"]["status"] == "closed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
