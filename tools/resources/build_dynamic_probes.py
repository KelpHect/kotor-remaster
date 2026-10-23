#!/usr/bin/env python3
"""Build a static probe manifest for lane 06's dynamic VFS measurement.

Read-only against the install.  It parses archive *tables* only: resrefs,
types, entry indices, file offsets and sizes.  It never reads resource
payloads and never writes to the install.

The manifest tells lane 06 which (resref, type) keys are discriminating,
which archive/offset/size the static five-pass model expects, and which parts
of that expectation are documented versus inferred.  Lane 06's hook log is
compared by ``vfs_measure.py``.

Usage:
  python build_dynamic_probes.py \
    --game-dir "<INSTALL>" --active-module m01aa \
    --save-folder "<SAVE>" \
    --out ../results/dynamic-probes.json
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for _p in (Path(__file__).resolve().parent, REPO_ROOT / "tools" / "inventory"):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

from kotor_vfs import (Vfs, DirectoryTable, ArchiveTable, KeyTable,
                       relative_label, type_label)  # noqa: E402


def _guard_out(path: Path) -> None:
    p = path.resolve()
    for parent in (p, *p.parents):
        if (parent / "chitin.key").exists() or (parent / "swkotor.exe").exists():
            raise SystemExit(f"refusing to write inside a game install: {p}")


def _tier(pass_no: int, table) -> str:
    if pass_no == 1:
        return f"directory:{table.name}"
    if pass_no == 2:
        return f"erf1:{table.name}"
    if pass_no == 3:
        return f"rim:{table.name}"
    if pass_no == 4:
        return f"erf2:{table.name}"
    return f"base:{table.name}"


def _row(pass_no: int, order: int, table, hit, game_dir: Path) -> dict:
    e = hit.entry
    return {
        "pass": pass_no,
        "tier": _tier(pass_no, table),
        "table": table.name,
        "table_kind": table.kind,
        "table_order": order,
        "entry_index": e.index,
        "size": e.size,
        "offset": e.offset,
        "source": relative_label(game_dir, Path(e.source)) if e.source else None,
        "duplicate_count": len(hit.duplicate_indices),
        "duplicate_indices": hit.duplicate_indices,
        "error": hit.error,
    }


def collect(v: Vfs, skip_texture_tables: bool = True) -> dict:
    """Return key -> [{table row...}] using only the first entry per table."""
    by_key: dict[tuple[str, int], list[dict]] = defaultdict(list)
    seen_keys_by_table: dict[int, set] = defaultdict(set)
    for p in v.passes:
        for order, table in enumerate(p.tables):
            if not table.exists:
                continue
            if skip_texture_tables and table.name.startswith("swpc_tex_"):
                continue
            seen = seen_keys_by_table[id(table)]
            for e in table.entries:
                if e.key in seen:
                    continue
                seen.add(e.key)
                try:
                    hit = table.lookup(e.key)
                except Exception:
                    continue
                if hit is None:
                    continue
                by_key[e.key].append(_row(p.number, order, table, hit, v.game_dir))
    return by_key


def _winner_info(v: Vfs, key) -> dict:
    res = v.trace(key[0], key[1])
    w = res.winner
    if w is None:
        return {"found": False}
    return {
        "found": True,
        "pass": w.pass_no,
        "tier": w.tier,
        "table": w.table.name,
        "entry_index": w.entry.index,
        "offset": w.entry.offset,
        "size": w.entry.size,
        "error": w.error,
        "traces": len(res.traces),
        "notes": res.notes[:3],
    }


def _class(sources: list[dict]) -> tuple[str, str]:
    passes = sorted({s["pass"] for s in sources})
    tables = sorted({s["table"] for s in sources})
    if any(s["table"].startswith("SAVEGAME.sav") for s in sources):
        return "save", "save-vs-module"
    if passes == [1] or (1 in passes and len(sources) > 1):
        return "override", "with-directory"
    if len(passes) == 1:
        return "intra-pass", f"p{passes[0]}"
    return "tier", "-".join(str(x) for x in passes)


def _authority(sources: list[dict], winner: dict) -> str:
    passes = {s["pass"] for s in sources}
    if len(passes) == 1 or (winner.get("pass") in passes and len(passes) == 1):
        return "inferred"
    # Cross-pass precedence is documented; save-vs-module and texture quality
    # remain inferred.
    if any(s["table"].startswith("SAVEGAME.sav") for s in sources):
        return "inferred"
    return "documented"


def build(game_dir: Path, active_module: str | None,
          save_folder: Path | None, max_per_class: int, max_total: int) -> dict:
    v = Vfs(game_dir, active_module=active_module, loaded_save=save_folder,
            quality="all")
    by_key = collect(v, skip_texture_tables=True)

    groups: dict[tuple[str, str], list] = defaultdict(list)
    for key, sources in by_key.items():
        if len(sources) < 2:
            continue
        if len({s["table"] for s in sources}) < 2:
            continue
        kind, sub = _class(sources)
        # Skip cases where the only other source is a duplicate in one table.
        groups[(kind, sub)].append((key, sources))

    probes = []
    for (kind, sub), items in sorted(groups.items()):
        items.sort(key=lambda kv: (kv[0][0], kv[0][1]))
        for key, sources in items[:max_per_class]:
            winner = _winner_info(v, key)
            if not winner.get("found"):
                continue
            probes.append({
                "id": f"{kind}.{sub}.{key[0]}.{key[1]}",
                "kind": kind,
                "class": sub,
                "key": {"resref": key[0], "type": key[1],
                        "type_label": type_label(key[1])},
                "sources": sources,
                "expected": winner,
                "authority": _authority(sources, winner),
                "observation": {
                    "method": "instrument the resource lookup; record pass/table/archive and entry offset/size",
                    "match": ["table", "offset", "size"],
                },
            })

    # Duplicate-key probes: first entry wins within one table.
    dup_probes = []
    for p in v.passes:
        for table in p.tables:
            if not table.exists or table.name.startswith("swpc_tex_"):
                continue
            for key, indices in list(getattr(table, "_dups", {}).items()):
                if len(indices) < 2:
                    continue
                hit = table.lookup(key)
                if hit is None:
                    continue
                dup_probes.append({
                    "id": f"duplicate.{table.name}.{key[0]}.{key[1]}",
                    "kind": "duplicate",
                    "key": {"resref": key[0], "type": key[1],
                            "type_label": type_label(key[1])},
                    "sources": [_row(p.number, 0, table, hit, v.game_dir)],
                    "expected": _winner_info(v, key),
                    "authority": "documented",
                    "observation": {
                        "method": "instrument one table; the first matching key must win",
                        "match": ["table", "entry_index"],
                    },
                })
                if len(dup_probes) >= max_total:
                    break
            if len(dup_probes) >= max_total:
                break
        if len(dup_probes) >= max_total:
            break

    # Texture-quality probes: same key in all three packs, with distinct sizes.
    texture_probes = []
    q_tables = {}
    for p in v.passes:
        for table in p.tables:
            if table.name in ("swpc_tex_tpa.erf", "swpc_tex_tpb.erf", "swpc_tex_tpc.erf"):
                q_tables[table.name] = table
    if len(q_tables) == 3:
        by_pack = {}
        for name, table in q_tables.items():
            by_pack[name] = {}
            for e in table.entries:
                by_pack[name].setdefault(e.key, table.lookup(e.key))
        common = set.intersection(*[set(x) for x in by_pack.values()])
        candidates = []
        for key in common:
            rows = []
            for name, table in q_tables.items():
                hit = by_pack[name].get(key)
                if hit is None:
                    rows = []
                    break
                rows.append({
                    "pack": name,
                    "table": name,
                    "entry_index": hit.entry.index,
                    "offset": hit.entry.offset,
                    "size": hit.entry.size,
                })
            if not rows:
                continue
            if len({r["size"] for r in rows}) == 1:
                continue
            candidates.append((key, rows))
        candidates.sort(key=lambda kv: (kv[0][0], kv[0][1]))
        for key, rows in candidates[:max_per_class]:
            texture_probes.append({
                "id": f"texture_quality.{key[0]}.{key[1]}",
                "kind": "texture_quality",
                "key": {"resref": key[0], "type": key[1],
                        "type_label": type_label(key[1])},
                "packs": rows,
                "authority": "unmeasured",
                "expected": {"quality": "unknown", "authority": "unmeasured",
                             "static_default": "tpa"},
                "observation": {
                    "method": "for each in-game texture quality setting, record the archive/offset/size used",
                    "match": ["table", "offset", "size"],
                },
            })

    probes.extend(dup_probes)
    probes.extend(texture_probes)
    probes = probes[:max_total]

    file_meta = []
    for name in ("chitin.key", "patch.erf", "swkotor.ini"):
        p = game_dir / name
        if p.exists():
            file_meta.append({"name": name, "size": p.stat().st_size})
    for p in sorted((game_dir / "TexturePacks").glob("*.erf")) if (game_dir / "TexturePacks").is_dir() else []:
        file_meta.append({"name": f"TexturePacks/{p.name}", "size": p.stat().st_size})
    override = game_dir / "Override"
    override_files = sum(1 for p in override.rglob("*") if p.is_file()) if override.is_dir() else 0

    return {
        "schema": "kotor-vfs-probes/1",
        "generated_unix": int(time.time()),
        "game": {
            "path": str(game_dir),
            "active_module": active_module,
            "save_folder": str(save_folder) if save_folder else None,
            "files": file_meta,
            "override_file_count": override_files,
        },
        "mounts": v.mounts(),
        "probes": probes,
        "counts": {
            "keys_total": len(by_key),
            "probes": len(probes),
            "tier": sum(1 for p in probes if p["kind"] == "tier"),
            "intra_pass": sum(1 for p in probes if p["kind"] == "intra-pass"),
            "duplicate": sum(1 for p in probes if p["kind"] == "duplicate"),
            "texture_quality": sum(1 for p in probes if p["kind"] == "texture_quality"),
            "save": sum(1 for p in probes if p["kind"] == "save"),
            "override": sum(1 for p in probes if p["kind"] == "override"),
        },
        "notes": [
            "Only archive tables/metadata were read; no resource payload was extracted.",
            "Intra-pass order and texture-quality mapping are inferred; see authority fields.",
            "Override is empty in the reference install, so dynamic Override measurement is blocked without an owner-approved isolated copy.",
            "Run vfs_measure.py compare with the lane 06 hook log.",
        ],
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game-dir", required=True)
    ap.add_argument("--active-module", default="m01aa")
    ap.add_argument("--save-folder")
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-per-class", type=int, default=4)
    ap.add_argument("--max-total", type=int, default=120)
    args = ap.parse_args(argv)
    game = Path(args.game_dir)
    save = Path(args.save_folder) if args.save_folder else None
    manifest = build(game, args.active_module, save, args.max_per_class,
                     args.max_total)
    out = Path(args.out)
    _guard_out(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps(manifest["counts"], indent=2))
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
