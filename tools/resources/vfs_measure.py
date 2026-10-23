#!/usr/bin/env python3
"""Compare a lane 06 resource-lookup log against static VFS probes.

The log is produced by lane 06's hook.  Expected schema, one JSON object per
line (a JSON array or {"events": [...]} is also accepted):

  {"event":"GetResource","resref":"patch_vs_rim","type":2010,
   "found":true,"pass":2,"table":"patch.erf","offset":1234,"size":45}

Only metadata is used.  Payload bytes are never read from the game.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for _p in (Path(__file__).resolve().parent, REPO_ROOT / "tools" / "inventory"):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

from kotor_vfs import normalize_resref, ext_to_type, type_label  # noqa: E402


def _guard_out(path: Path) -> None:
    p = path.resolve()
    for parent in (p, *p.parents):
        if (parent / "chitin.key").exists() or (parent / "swkotor.exe").exists():
            raise SystemExit(f"refusing to write inside a game install: {p}")


def _first(obj: dict, names, default=None):
    for name in names:
        if name in obj:
            return obj[name]
    return default


def normalize_event(obj: dict) -> dict | None:
    if not isinstance(obj, dict):
        return None
    resref = _first(obj, ("resref", "ResRef", "resRef", "resource", "name"))
    if not isinstance(resref, str):
        return None
    rtype = _first(obj, ("type", "res_type", "resType", "ResType",
                         "resource_type", "ResourceType"))
    if isinstance(rtype, str):
        rtype = int(rtype) if rtype.isdigit() else ext_to_type(rtype)
    if rtype is None and "." in resref:
        stem, ext = resref.rsplit(".", 1)
        typ = ext_to_type(ext)
        if typ is not None:
            resref, rtype = stem, typ
    if rtype is None:
        return None
    table = _first(obj, ("table", "Table", "archive", "Archive", "source",
                         "file", "File", "archive_path", "ArchivePath",
                         "container"))
    pass_no = _first(obj, ("pass", "Pass", "pass_no", "PassNo", "tier_pass"))
    try:
        pass_no = int(pass_no) if pass_no is not None else None
    except (TypeError, ValueError):
        pass_no = None
    found = _first(obj, ("found", "Found", "hit", "success"), True)
    if isinstance(found, str):
        found = found.lower() not in ("false", "0", "no")
    return {
        "event": _first(obj, ("event", "Event", "event_type", "action"),
                        "lookup"),
        "resref": normalize_resref(resref),
        "type": int(rtype),
        "found": bool(found),
        "pass": pass_no,
        "table": str(table) if table is not None else None,
        "offset": _first(obj, ("offset", "Offset", "file_offset",
                               "FileOffset", "entry_offset")),
        "size": _first(obj, ("size", "Size", "resource_size", "ResourceSize",
                             "entry_size")),
        "entry_index": _first(obj, ("entry_index", "EntryIndex", "index",
                                    "key_index")),
        "timestamp": _first(obj, ("timestamp", "Timestamp", "ts", "time")),
    }


def load_events(path: Path) -> list[dict]:
    text = path.read_text(encoding="utf-8", errors="replace")
    if not text.strip():
        return []
    try:
        obj = json.loads(text)
    except json.JSONDecodeError:
        obj = None
    if isinstance(obj, list):
        items = obj
    elif isinstance(obj, dict) and isinstance(obj.get("events"), list):
        items = obj["events"]
    elif isinstance(obj, dict):
        items = [obj]
    else:
        items = []
        for line in text.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                items.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    out = []
    for item in items:
        ev = normalize_event(item)
        if ev is not None:
            out.append(ev)
    return out


def _norm_table(value: str | None) -> str:
    if not value:
        return ""
    return str(value).replace("\\", "/").lower()


def table_matches(expected: str, observed: str | None) -> bool:
    e = _norm_table(expected)
    o = _norm_table(observed)
    if not e or not o:
        return False
    if e == o:
        return True
    if "#" in e:
        base, suffix = e.split("#", 1)
        return os.path.basename(base) in o and suffix in o
    if o.endswith("/" + e) or o.endswith(e):
        return True
    return e in o


def source_match(event: dict, source: dict) -> bool:
    if table_matches(source.get("table", ""), event.get("table")):
        return True
    src = source.get("source")
    if src and table_matches(os.path.basename(src), event.get("table")):
        return True
    # If the log has only offset/size, match on those.
    if event.get("size") is not None and source.get("size") == event["size"]:
        if event.get("offset") is None or source.get("offset") == event["offset"]:
            return True
    return False


def infer_from_sources(event: dict, sources: list[dict]) -> dict | None:
    candidates = [s for s in sources if source_match(event, s)]
    if not candidates:
        return None
    # Prefer an exact table match over a size-only match.
    exact = [s for s in candidates if table_matches(s.get("table", ""), event.get("table"))]
    return (exact or candidates)[0]


def compare(probes: dict, events: list[dict]) -> dict:
    by_key: dict[tuple[str, int], list[dict]] = {}
    for e in events:
        by_key.setdefault((e["resref"], e["type"]), []).append(e)
    rows = []
    counts = Counter()
    quality_counts = Counter()
    for probe in probes.get("probes", []):
        key = (normalize_resref(probe["key"]["resref"]), int(probe["key"]["type"]))
        observed = by_key.get(key, [])
        expected = probe.get("expected", {})
        sources = probe.get("sources", [])
        if not observed:
            rows.append({"id": probe.get("id"), "key": probe.get("key"),
                         "kind": probe.get("kind"), "status": "unobserved",
                         "expected": expected})
            counts["unobserved"] += 1
            continue
        if probe.get("kind") == "texture_quality":
            packs = {os.path.basename(p["pack"]).lower(): p for p in probe.get("packs", [])}
            hit = None
            for ev in observed:
                for name in packs:
                    if table_matches(name, ev.get("table")):
                        hit = (name, ev)
                        break
                if hit:
                    break
            if hit is None:
                status = "ambiguous"
                counts[status] += 1
                rows.append({"id": probe.get("id"), "key": probe.get("key"),
                             "kind": "texture_quality", "status": status,
                             "events": observed})
            else:
                name, ev = hit
                quality = name.replace("swpc_tex_", "").replace(".erf", "")
                quality_counts[quality] += 1
                rows.append({"id": probe.get("id"), "key": probe.get("key"),
                             "kind": "texture_quality", "status": "observed",
                             "observed_quality": quality,
                             "observed": ev,
                             "packs": probe.get("packs")})
                counts["observed"] += 1
            continue

        results = []
        for ev in observed:
            source = infer_from_sources(ev, sources)
            obs_pass = ev.get("pass")
            if obs_pass is None and source is not None:
                obs_pass = source.get("pass")
            obs_table = ev.get("table") or (source or {}).get("table")
            checks = []
            if expected.get("found", True) != ev.get("found", True):
                checks.append(("found", expected.get("found"), ev.get("found")))
            if expected.get("pass") is not None and obs_pass is not None:
                checks.append(("pass", expected.get("pass"), obs_pass))
            if expected.get("table") and obs_table:
                checks.append(("table", expected.get("table"), obs_table))
            if expected.get("offset") is not None and ev.get("offset") is not None:
                checks.append(("offset", expected.get("offset"), ev.get("offset")))
            if expected.get("size") is not None and ev.get("size") is not None:
                checks.append(("size", expected.get("size"), ev.get("size")))
            ok = True
            for field, want, got in checks:
                if field == "table":
                    table_ok = table_matches(str(want), str(got))
                    if not table_ok and source is not None and want == source.get("table"):
                        # chitin.key is the index; the observed archive may be a BIF.
                        table_ok = table_matches(source.get("source") or "", str(got))
                    ok = ok and table_ok
                else:
                    ok = ok and (want == got)
            results.append({"ok": ok, "pass": obs_pass, "table": obs_table,
                            "offset": ev.get("offset"), "size": ev.get("size"),
                            "checks": checks})
        if all(r["ok"] for r in results):
            status = "match"
        elif any(r["ok"] for r in results) and len(results) > 1:
            status = "ambiguous"
        else:
            status = "mismatch"
        counts[status] += 1
        rows.append({
            "id": probe.get("id"), "key": probe.get("key"),
            "kind": probe.get("kind"), "authority": probe.get("authority"),
            "status": status,
            "expected": expected,
            "observed": results,
            "events": observed,
        })
    return {
        "schema": "kotor-vfs-measure/1",
        "probes": len(probes.get("probes", [])),
        "events": len(events),
        "counts": dict(counts),
        "texture_quality_counts": dict(quality_counts),
        "results": rows,
        "notes": [
            "match means the observed pass/table/offset/size agree with the static model",
            "ambiguous means multiple log events disagree; inspect lane 06 instrumentation",
            "texture_quality rows are observations; expected quality remains unmeasured",
        ],
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    cp = sub.add_parser("compare")
    cp.add_argument("--probes", required=True)
    cp.add_argument("--log", required=True)
    cp.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    if args.cmd == "compare":
        probes = json.loads(Path(args.probes).read_text(encoding="utf-8"))
        events = load_events(Path(args.log))
        report = compare(probes, events)
        out = Path(args.out)
        _guard_out(out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps({"counts": report["counts"],
                          "texture_quality_counts": report["texture_quality_counts"]},
                         indent=2))
        print(f"wrote {out}")
        mismatches = report["counts"].get("mismatch", 0)
        return 1 if mismatches else 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
