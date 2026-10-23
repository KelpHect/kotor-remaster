#!/usr/bin/env python3
"""Read-only inventory of a KotOR installation's archives.

Parses chitin.key, the BIF headers it references, and the RIM modules under
modules/. Never writes to the game directory. Outputs a JSON report and a
short human-readable summary.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone


def read_exact(f, offset: int, size: int) -> bytes:
    f.seek(offset)
    data = f.read(size)
    if len(data) != size:
        raise ValueError(f"short read at 0x{offset:x}: wanted {size}, got {len(data)}")
    return data


def sha256_file(path: str, limit: int | None = None) -> str:
    h = hashlib.sha256()
    remaining = limit
    with open(path, "rb") as f:
        while True:
            chunk = f.read(min(1 << 20, remaining) if remaining else 1 << 20)
            if not chunk:
                break
            h.update(chunk)
            if remaining is not None:
                remaining -= len(chunk)
                if remaining <= 0:
                    break
    return h.hexdigest()


def file_meta(path: str) -> dict:
    st = os.stat(path)
    return {
        "name": os.path.basename(path),
        "size": st.st_size,
        "mtime_utc": datetime.fromtimestamp(st.st_mtime, timezone.utc).isoformat(),
    }


def parse_key(path: str) -> dict:
    with open(path, "rb") as f:
        head = f.read(64)
        if head[:4] != b"KEY " or head[4:8] != b"V1  ":
            raise ValueError(f"{path}: not a KEY V1 file")
        bif_count, key_count, off_files, off_keys = struct.unpack_from("<4I", head, 8)

        files = []
        table = read_exact(f, off_files, bif_count * 12)
        names_blob = read_exact(f, off_files, min(head_size_guess := 64 * 1024, os.path.getsize(path) - off_files))
        for i in range(bif_count):
            size, name_off, name_len, drives = struct.unpack_from("<IIHH", table, i * 12)
            # Name offsets are relative to the file table start in known KEY files;
            # fall back to absolute offsets for tolerance.
            cand = names_blob[name_off:name_off + name_len]
            if not cand or b"\x00" in cand or not cand.lower().endswith(b".bif"):
                cand = names_blob[max(0, name_off - off_files):max(0, name_off - off_files) + name_len]
            name = cand.split(b"\x00", 1)[0].decode("latin-1", "replace")
            files.append({"index": i, "declared_size": size, "name": name, "drives": drives})

        key_blob = read_exact(f, off_keys, os.path.getsize(path) - off_keys)
        # KotOR/NWN key entries are 22 bytes packed. Detect an exact 24-byte
        # stride as a fallback (some tools write padded tables).
        stride = 22
        if key_count and len(key_blob) % key_count == 0:
            padded = len(key_blob) // key_count
            if padded == 24:
                stride = 24
        entries = []
        for i in range(key_count):
            base = i * stride
            if base + 22 > len(key_blob):
                raise ValueError("key table truncated")
            raw = key_blob[base:base + 16]
            resref = raw.split(b"\x00", 1)[0].decode("latin-1", "replace")
            rtype, resid = struct.unpack_from("<HI", key_blob, base + 16)
            entries.append({
                "resref": resref,
                "type": rtype,
                "id": resid,
                "bif": (resid >> 20) & 0xFFF,
                "index": resid & 0xFFFFF,
            })

    return {
        "path": path,
        "sha256": sha256_file(path),
        "bif_count": bif_count,
        "key_count": key_count,
        "off_files": off_files,
        "off_keys": off_keys,
        "stride": stride,
        "bifs": files,
        "entries": entries,
    }


def parse_bif(path: str) -> dict:
    with open(path, "rb") as f:
        head = f.read(20)
        if head[:4] != b"BIFF" or head[4:8] != b"V1  ":
            raise ValueError(f"{path}: not a BIFF V1 file")
        var_count, fixed_count, off_var = struct.unpack_from("<3I", head, 8)
        size = os.path.getsize(path)
        off_fixed = off_var + var_count * 16
        if off_var < 20 or off_var > size:
            raise ValueError(f"{path}: variable table offset outside file")
        if var_count and off_var + var_count * 16 > size:
            raise ValueError(f"{path}: variable table exceeds file")
        if fixed_count and off_fixed + fixed_count * 16 > size:
            raise ValueError(f"{path}: fixed table exceeds file")

        def entry_at(off: int) -> dict:
            rid, roff, res_size, rtype = struct.unpack("<4I", read_exact(f, off, 16))
            if res_size and roff + res_size > size:
                raise ValueError(f"{path}: resource at 0x{off:x} exceeds file")
            return {"id": rid, "offset": roff, "size": res_size, "type": rtype}

        fixed = [entry_at(off_fixed + i * 16) for i in range(fixed_count)]
        var = [entry_at(off_var + i * 16) for i in range(var_count)]
    return {
        "name": os.path.basename(path),
        "size": size,
        "var_count": var_count,
        "fixed_count": fixed_count,
        "off_var": off_var,
        "off_fixed": off_fixed,
        "fixed": fixed,
        "var": var,
    }


RIM_ENTRY_CANDIDATES = (
    ("<16sIIII", 32),  # KotOR RIM: resref, type, id, offset, size
    ("<16sHHII", 28),  # TSL-ish variant
)


def parse_rim(path: str) -> dict | None:
    with open(path, "rb") as f:
        head = f.read(20)
        if len(head) < 20 or head[:8] != b"RIM V1.0":
            return None
        version = head[4:8].decode("latin-1", "replace").strip()
        entry_count, off_entries = struct.unpack_from("<2I", head, 12)
        size = os.path.getsize(path)
        chosen = None
        for fmt, rec_size in RIM_ENTRY_CANDIDATES:
            if off_entries + entry_count * rec_size > size:
                continue
            entries = []
            ok = True
            for i in range(entry_count):
                blob = read_exact(f, off_entries + i * rec_size, rec_size)
                resref = blob[:16].split(b"\x00", 1)[0].decode("latin-1", "replace")
                fields = struct.unpack(fmt, blob)
                if rec_size == 32:
                    rtype, rid, off, res_size = fields[1], fields[2], fields[3], fields[4]
                else:
                    rtype, rid, off, res_size = fields[1], fields[2], fields[3], fields[4]
                if off and res_size and off + res_size > size:
                    ok = False
                    break
                entries.append({"resref": resref, "type": rtype, "id": rid, "offset": off, "size": res_size})
            if ok:
                chosen = {"version": version, "entry_count": entry_count, "off_entries": off_entries,
                          "record_size": rec_size, "entries": entries}
                break
    return chosen


def parse_erf(path: str) -> dict | None:
    """Parse ERF/MOD/SAV. KotOR's ERF header places entryCount at 0x10;
    NWN-derived tooling often expects 0x0C. Try both and validate the tables."""
    with open(path, "rb") as f:
        head = f.read(32)
        if head[:4] not in (b"ERF ", b"MOD ", b"SAV "):
            return None
        size = os.path.getsize(path)
        for count_off, key_off, res_off, layout in (
            (0x10, 0x14, 0x1C, "kotor-v1"),
            (0x0C, 0x10, 0x18, "nwn-v1"),
        ):
            count = struct.unpack_from("<I", head, count_off)[0]
            off_key = struct.unpack_from("<I", head, key_off)[0]
            off_res = struct.unpack_from("<I", head, res_off)[0]
            if not count or count > 1_000_000:
                continue
            if off_key + count * 24 > size or off_res + count * 8 > size:
                continue
            try:
                keys = []
                for i in range(count):
                    b = read_exact(f, off_key + i * 24, 24)
                    resref = b[:16].split(b"\x00", 1)[0].decode("latin-1", "replace")
                    if not resref or any(not (32 <= ord(c) < 127) for c in resref):
                        raise ValueError("non-printable resref")
                    rid, rtype, _unused = struct.unpack_from("<IHH", b, 16)
                    keys.append({"resref": resref, "id": rid, "type": rtype})
                resources = []
                for i in range(count):
                    off, rsize = struct.unpack_from("<II", read_exact(f, off_res + i * 8, 8))
                    if off + rsize > size:
                        raise ValueError("resource outside file")
                    resources.append({"offset": off, "size": rsize})
            except Exception:
                continue
            for k, r in zip(keys, resources):
                k.update(r)
            return {"path": path, "layout": layout, "entry_count": count, "entries": keys}
    return None


def erf_summary(path: str) -> dict | None:
    erf = parse_erf(path)
    if not erf:
        return None
    by_type: Counter = Counter(e["type"] for e in erf["entries"])
    return {
        "name": os.path.relpath(path, os.path.dirname(path)),
        "file": os.path.basename(path),
        "size": os.path.getsize(path),
        "layout": erf["layout"],
        "entry_count": erf["entry_count"],
        "by_type": dict(sorted(by_type.items())),
        "entry_bytes": sum(e["size"] for e in erf["entries"]),
        "sample": [e["resref"] for e in erf["entries"][:8]],
    }


def collect(game_dir: str, include_modules: bool = True) -> dict:
    key_path = os.path.join(game_dir, "chitin.key")
    key = parse_key(key_path)
    report: dict = {
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "game_dir": os.path.abspath(game_dir),
        "key": {k: v for k, v in key.items() if k not in ("entries", "bifs")},
        "bifs": [],
        "resources": {
            "total": len(key["entries"]),
            "by_type": dict(sorted(Counter(e["type"] for e in key["entries"]).items())),
        },
        "modules": {"dir": "modules", "count": 0, "by_type": {}, "total_entry_bytes": 0, "files": []},
    }

    # Verify each referenced BIF and reconcile key entries against BIF tables.
    key_by_bif = defaultdict(list)
    for e in key["entries"]:
        key_by_bif[e["bif"]].append(e)
    for bif_meta in key["bifs"]:
        name = bif_meta["name"].replace("\\", "/")
        rel = name if name.lower().startswith("data/") else os.path.join("data", os.path.basename(name))
        path = os.path.join(game_dir, rel)
        row = {"index": bif_meta["index"], "name": name, "declared_size": bif_meta["declared_size"],
               "exists": os.path.exists(path)}
        if row["exists"]:
            try:
                bif = parse_bif(path)
                row.update({
                    "size": bif["size"],
                    "var_count": bif["var_count"],
                    "fixed_count": bif["fixed_count"],
                    "table_resources": bif["var_count"] + bif["fixed_count"],
                    "key_entries": len(key_by_bif.get(bif_meta["index"], [])),
                    "top_types": dict(Counter([r["type"] for r in bif["fixed"] + bif["var"]]).most_common(8)),
                })
            except Exception as exc:  # keep the inventory going; record the failure
                row["error"] = str(exc)
        report["bifs"].append(row)

    # Aggregate resource bytes from BIF tables (all shipped base resources).
    type_bytes: Counter = Counter()
    type_counts: Counter = Counter()
    for bif_meta in key["bifs"]:
        name = bif_meta["name"].replace("\\", "/")
        path = os.path.join(game_dir, name if name.lower().startswith("data/") else os.path.join("data", os.path.basename(name)))
        if not os.path.exists(path):
            continue
        try:
            bif = parse_bif(path)
        except Exception:
            continue
        for r in bif["fixed"] + bif["var"]:
            type_bytes[r["type"]] += r["size"]
            type_counts[r["type"]] += 1
    report["resources"]["by_type_counts_from_bif"] = dict(sorted(type_counts.items()))
    report["resources"]["by_type_bytes"] = dict(sorted(type_bytes.items()))

    if include_modules:
        mod_dir = os.path.join(game_dir, "modules")
        by_type: Counter = Counter()
        total_bytes = 0
        if os.path.isdir(mod_dir):
            for fn in sorted(os.listdir(mod_dir)):
                if not fn.lower().endswith(".rim"):
                    continue
                path = os.path.join(mod_dir, fn)
                try:
                    rim = parse_rim(path)
                except Exception as exc:
                    report["modules"]["files"].append({"name": fn, "error": str(exc)})
                    continue
                if not rim:
                    report["modules"]["files"].append({"name": fn, "error": "unrecognized RIM"})
                    continue
                entry_bytes = sum(e["size"] for e in rim["entries"])
                total_bytes += entry_bytes
                types = Counter(e["type"] for e in rim["entries"])
                by_type.update(types)
                report["modules"]["files"].append({
                    "name": fn,
                    "size": os.path.getsize(path),
                    "entry_count": rim["entry_count"],
                    "entry_bytes": entry_bytes,
                    "top_types": dict(types.most_common(8)),
                })
        report["modules"]["count"] = len([f for f in report["modules"]["files"] if "error" not in f])
        report["modules"]["by_type"] = dict(sorted(by_type.items()))
        report["modules"]["total_entry_bytes"] = total_bytes

    report["erf_archives"] = []
    erf_paths = [os.path.join(game_dir, "patch.erf")]
    tp_dir = os.path.join(game_dir, "TexturePacks")
    if os.path.isdir(tp_dir):
        erf_paths += [os.path.join(tp_dir, fn) for fn in sorted(os.listdir(tp_dir)) if fn.lower().endswith(".erf")]
    for path in erf_paths:
        if not os.path.exists(path):
            continue
        row = erf_summary(path)
        if row:
            report["erf_archives"].append(row)

    report["global_rims"] = {"dir": "rims", "count": 0, "by_type": {}, "entry_bytes": 0, "files": []}
    rims_dir = os.path.join(game_dir, "rims")
    if os.path.isdir(rims_dir):
        by_type = Counter()
        for fn in sorted(os.listdir(rims_dir)):
            if not fn.lower().endswith(".rim"):
                continue
            path = os.path.join(rims_dir, fn)
            try:
                rim = parse_rim(path)
            except Exception as exc:
                report["global_rims"]["files"].append({"name": fn, "error": str(exc)})
                continue
            if not rim:
                report["global_rims"]["files"].append({"name": fn, "error": "unrecognized RIM"})
                continue
            types = Counter(e["type"] for e in rim["entries"])
            by_type.update(types)
            report["global_rims"]["files"].append({
                "name": fn,
                "size": os.path.getsize(path),
                "entry_count": rim["entry_count"],
                "entry_bytes": sum(e["size"] for e in rim["entries"]),
                "top_types": dict(types.most_common(8)),
            })
            report["global_rims"]["count"] += 1
            report["global_rims"]["entry_bytes"] += sum(e["size"] for e in rim["entries"])
        report["global_rims"]["by_type"] = dict(sorted(by_type.items()))

    report["manifest"] = build_manifest(game_dir)
    return report


def build_manifest(game_dir: str) -> list[dict]:
    names = ["swkotor.exe", "chitin.key", "dialog.tlk", "patch.erf", "swkotor.ini"]
    out = []
    for name in names:
        path = os.path.join(game_dir, name)
        if not os.path.exists(path):
            continue
        meta = file_meta(path)
        meta["sha256"] = sha256_file(path)
        out.append(meta)
    return out


def summarize(report: dict) -> str:
    from kotor_types import label

    lines = []
    r = report["resources"]
    lines.append(f"Game: {report['game_dir']}")
    lines.append(f"KEY: {report['key']['key_count']} resources, {report['key']['bif_count']} BIFs, stride {report['key']['stride']}")
    labeled = ", ".join(f"{label(t)}:{n}" for t, n in r["by_type"].items())
    lines.append(f"Base resources by type: {labeled}")
    lines.append("Top BIF types (by count):")
    for b in report["bifs"]:
        if "key_entries" in b:
            lines.append(f"  {b['name']:<24} keys={b['key_entries']:<5} tables={b['table_resources']:<5} top={b.get('top_types')}")
        else:
            lines.append(f"  {b['name']:<24} missing or unparsed: {b.get('error', 'not found')}")
    m = report["modules"]
    lines.append(f"Modules: {m['count']} RIM files, {m['total_entry_bytes']} entry bytes, types={m['by_type']}")
    g = report.get("global_rims", {})
    if g:
        lines.append(f"Global RIMs: {g['count']} files, {g['entry_bytes']} entry bytes, types={g['by_type']}")
    for e in report.get("erf_archives", []):
        lines.append(f"ERF {e['file']:<24} entries={e['entry_count']:<5} layout={e['layout']:<9} "
                     f"by_type={e['by_type']} sample={e['sample'][:4]}")
    lines.append("Manifest:")
    for f in report["manifest"]:
        lines.append(f"  {f['name']:<12} {f['size']:>10} {f['sha256'][:16]}…")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game-dir", required=True, help="KotOR install directory containing chitin.key")
    ap.add_argument("--out", default="artifacts/inventory.json", help="JSON output path")
    ap.add_argument("--no-modules", action="store_true", help="skip RIM module scan")
    args = ap.parse_args()

    report = collect(args.game_dir, include_modules=not args.no_modules)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(summarize(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
