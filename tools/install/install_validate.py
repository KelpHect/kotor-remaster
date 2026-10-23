#!/usr/bin/env python3
"""Read-only detection and validation of a KotOR 1 (K1) installation.

Standard library only. The validator never writes to the install, never
launches the game, and never requires ``swkotor.exe``.

It establishes install identity from data files alone:

* required files: ``chitin.key``, ``dialog.tlk``, ``patch.erf``
* required directories: ``data/`` (with at least one ``*.bif``), ``modules/``,
  ``rims/``
* KEY/BIFF structure: header counts, BIF table bounds, BIF sizes declared by the
  KEY, and KEY entry counts reconciled against the BIFF resource tables
* ``dialog.tlk`` header: version/language/string count and entry-table bounds
* product version/language: TLK language id plus a content fingerprint
  (chitin.key SHA-256) matched against the reference dataset captured in
  docs/SPRINT0_REPORT.md. No executable is read.

Exit codes: 0 valid, 2 invalid, 3 usage/IO error.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from collections import Counter

SCHEMA = "kotor-install-validation/1"

REQUIRED_FILES = ("chitin.key", "dialog.tlk", "patch.erf")
REQUIRED_DIRS = ("data", "modules", "rims")
EXE_NAMES = ("swkotor.exe",)

# Reference install observed by Sprint 0 (docs/recon/06_MANIFEST.md,
# docs/SPRINT0_REPORT.md). Matched by chitin.key SHA-256 so no install path is
# embedded. The executable is deliberately not used for version detection.
REFERENCE_DATASETS = {
    "2029ec1ee3f0e78a951320a7d9e3cfd0c21fc7ad6ec875e5de59a75f1c62df98": {
        "id": "kotor1-pc-1.0.3.0-english",
        "product_version": "1.0.3.0",
        "version_source": "chitin.key SHA-256 equals the Sprint 0 reference dataset; swkotor.exe not read",
        "language": "english",
        "language_id": 0,
        "app_id": 32370,
        "provenance": "docs/SPRINT0_REPORT.md, docs/recon/06_MANIFEST.md",
    }
}

LANGUAGES = {
    0: "english",
    1: "french",
    2: "german",
    3: "italian",
    4: "spanish",
    5: "polish",
}

KEY_BIF_RECORD = struct.Struct("<IIHH")  # size, name_off, name_len, drives
KEY_ENTRY_RECORD = 22  # 16-byte resref + u16 type + u32 resource id
BIF_ENTRY_RECORD = struct.Struct("<4I")  # id, offset, size, type
TLK_HEADER = 20
TLK_ENTRY_RECORD = 40


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _issue(code: str, severity: str, message: str, path: str | None = None) -> dict:
    row = {"code": code, "severity": severity, "message": message}
    if path is not None:
        row["path"] = path
    return row


def parse_key(path: str) -> dict:
    """Parse chitin.key: header, BIF table and resource-entry table."""
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < 64 or blob[:4] != b"KEY " or blob[4:8] != b"V1  ":
        raise ValueError("not a KEY V1 file")
    bif_count, key_count, off_files, off_keys = struct.unpack_from("<4I", blob, 8)
    table_end = off_files + bif_count * KEY_BIF_RECORD.size
    if off_files < 64 or table_end > len(blob) or off_keys > len(blob):
        raise ValueError("KEY tables outside file")
    if off_keys + key_count * KEY_ENTRY_RECORD > len(blob):
        raise ValueError("KEY entry table truncated")

    bifs = []
    for i in range(bif_count):
        size, name_off, name_len, drives = KEY_BIF_RECORD.unpack_from(
            blob, off_files + i * KEY_BIF_RECORD.size
        )
        # Name offsets are absolute file offsets (verified against this KEY).
        if name_off + name_len > len(blob):
            raise ValueError(f"BIF name outside file (record {i})")
        name = blob[name_off:name_off + name_len]
        name = name.split(b"\x00", 1)[0].decode("latin-1", "replace")
        bifs.append({
            "index": i,
            "name": name,
            "declared_size": size,
            "drives": drives,
        })

    types: Counter = Counter()
    per_bif: Counter = Counter()
    for i in range(key_count):
        base = off_keys + i * KEY_ENTRY_RECORD
        rtype = struct.unpack_from("<H", blob, base + 16)[0]
        resid = struct.unpack_from("<I", blob, base + 18)[0]
        types[rtype] += 1
        per_bif[(resid >> 20) & 0xFFF] += 1

    return {
        "bif_count": bif_count,
        "key_count": key_count,
        "off_files": off_files,
        "off_keys": off_keys,
        "entry_stride": KEY_ENTRY_RECORD,
        "bifs": bifs,
        "type_counts": dict(sorted(types.items())),
        "entries_per_bif": dict(sorted(per_bif.items())),
        "sha256": hashlib.sha256(blob).hexdigest(),
    }


def parse_bif(path: str) -> dict:
    """Parse a BIFF V1 header and its resource tables."""
    with open(path, "rb") as f:
        head = f.read(20)
        if len(head) != 20 or head[:4] != b"BIFF" or head[4:8] != b"V1  ":
            raise ValueError("not a BIFF V1 file")
        var_count, fixed_count, off_var = struct.unpack_from("<3I", head, 8)
        size = os.path.getsize(path)
        off_fixed = off_var + var_count * BIF_ENTRY_RECORD.size
        if off_var < 20 or off_var > size:
            raise ValueError("variable table offset outside file")
        if off_fixed + fixed_count * BIF_ENTRY_RECORD.size > size:
            raise ValueError("resource table exceeds file")
        end = off_fixed + fixed_count * BIF_ENTRY_RECORD.size
        f.seek(off_var)
        table = f.read(end - off_var)
    for row in BIF_ENTRY_RECORD.iter_unpack(table):
        _rid, off, res_size, _rtype = row
        if res_size and off + res_size > size:
            raise ValueError("resource payload outside file")
    return {
        "size": size,
        "var_count": var_count,
        "fixed_count": fixed_count,
        "table_resources": var_count + fixed_count,
        "off_var": off_var,
    }


def parse_tlk(path: str) -> dict:
    """Parse the dialog.tlk header (TLK V3.0)."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(TLK_HEADER)
    if len(head) < TLK_HEADER or head[:8] != b"TLK V3.0":
        raise ValueError("not a TLK V3.0 file")
    language_id, string_count, entries_offset = struct.unpack_from("<3I", head, 8)
    expected_offset = TLK_HEADER + string_count * TLK_ENTRY_RECORD
    issues = []
    if entries_offset != expected_offset:
        issues.append(_issue(
            "tlk_entry_offset",
            "warning",
            f"stringEntriesOffset {entries_offset} != 20 + {string_count}*40 = {expected_offset}",
        ))
    if entries_offset > size:
        raise ValueError("string table offset outside file")
    return {
        "language_id": language_id,
        "language": LANGUAGES.get(language_id, f"unknown({language_id})"),
        "string_count": string_count,
        "entries_offset": entries_offset,
        "file_size": size,
        "sha256": sha256_file(path),
        "issues": issues,
    }


def _rel(root: str, path: str) -> str:
    return os.path.relpath(path, root).replace("\\", "/")


def detect_install(start: str) -> dict:
    """Locate an install root from a directory, a file, or a parent directory.

    Returns {"root": str|None, "candidates": [...], "searched": [...]}.
    """
    start = os.path.abspath(start)
    searched = []
    candidates = []
    if os.path.isfile(start):
        start = os.path.dirname(start)

    def consider(d: str) -> None:
        searched.append(d)
        if os.path.isfile(os.path.join(d, "chitin.key")):
            candidates.append(d)

    if not os.path.isdir(start):
        return {"root": None, "candidates": [], "searched": [start]}

    consider(start)
    parent = os.path.dirname(start)
    if parent and parent != start:
        consider(parent)
    try:
        children = sorted(os.listdir(start))
    except OSError:
        children = []
    for name in children:
        child = os.path.join(start, name)
        if os.path.isdir(child):
            consider(child)
    root = candidates[0] if candidates else None
    return {"root": root, "candidates": candidates, "searched": searched}


def validate_install(game_dir: str, expect: dict[str, str] | None = None) -> dict:
    """Validate an install without writing to it.

    ``expect`` maps install-relative POSIX paths to expected SHA-256 values.
    """
    root = os.path.abspath(game_dir)
    issues: list[dict] = []
    result: dict = {
        "schema": SCHEMA,
        "install": {
            "root": root,
            "exists": os.path.isdir(root),
            "executable_present": any(os.path.isfile(os.path.join(root, n)) for n in EXE_NAMES),
        },
        "required": {},
        "key": None,
        "bifs": [],
        "tlk": None,
        "archives": {},
        "dataset": {},
        "issues": issues,
        "ok": False,
    }

    if not os.path.isdir(root):
        issues.append(_issue("install_missing", "error", "install directory does not exist", root))
        return result

    # Required files.
    for name in REQUIRED_FILES:
        path = os.path.join(root, name)
        present = os.path.isfile(path)
        result["required"][name] = {"present": present}
        if present:
            result["required"][name]["size"] = os.path.getsize(path)
        else:
            issues.append(_issue("missing_required_file", "error", f"missing {name}", name))

    # Required directories.
    for name in REQUIRED_DIRS:
        path = os.path.join(root, name)
        present = os.path.isdir(path)
        result["required"][name + "/"] = {"present": present}
        if not present:
            issues.append(_issue("missing_required_dir", "error", f"missing directory {name}/", name + "/"))

    # data/*.bif
    data_dir = os.path.join(root, "data")
    bif_paths = []
    if os.path.isdir(data_dir):
        bif_paths = sorted(
            os.path.join(data_dir, n) for n in os.listdir(data_dir) if n.lower().endswith(".bif")
        )
    result["required"]["data/*.bif"] = {"present": bool(bif_paths), "count": len(bif_paths)}
    if not bif_paths:
        issues.append(_issue("missing_bif_files", "error", "no data/*.bif files"))

    # ---- KEY ----
    key_path = os.path.join(root, "chitin.key")
    key = None
    if os.path.isfile(key_path):
        try:
            key = parse_key(key_path)
            result["key"] = {k: v for k, v in key.items() if k not in ("bifs", "type_counts", "entries_per_bif")}
            result["key"]["type_counts"] = key["type_counts"]
            result["key"]["bif_names"] = [b["name"] for b in key["bifs"]]
        except Exception as exc:
            issues.append(_issue("key_parse", "error", f"chitin.key unreadable: {exc}", "chitin.key"))

    # ---- BIFFs declared by KEY (plus any unreferenced data/*.bif) ----
    if key:
        declared = []
        for meta in key["bifs"]:
            name = meta["name"].replace("\\", "/")
            rel = name if name.lower().startswith("data/") else "data/" + os.path.basename(name)
            declared.append((meta, rel))
        for meta, rel in declared:
            path = os.path.join(root, *rel.split("/"))
            row = {
                "index": meta["index"],
                "name": meta["name"],
                "declared_size": meta["declared_size"],
                "exists": os.path.isfile(path),
                "key_entries": key["entries_per_bif"].get(meta["index"], 0),
            }
            if not row["exists"]:
                issues.append(_issue("bif_missing", "error", f"KEY references missing BIF {meta['name']}", rel))
                result["bifs"].append(row)
                continue
            row["size"] = os.path.getsize(path)
            if meta["declared_size"] != row["size"]:
                issues.append(_issue(
                    "bif_size_mismatch",
                    "error",
                    f"KEY declares {meta['declared_size']} B, file is {row['size']} B",
                    rel,
                ))
            try:
                bif = parse_bif(path)
                row.update({k: bif[k] for k in ("var_count", "fixed_count", "table_resources", "off_var")})
                if bif["table_resources"] != row["key_entries"]:
                    issues.append(_issue(
                        "bif_entry_count_mismatch",
                        "error",
                        f"BIF tables hold {bif['table_resources']} resources, KEY lists {row['key_entries']}",
                        rel,
                    ))
            except Exception as exc:
                issues.append(_issue("bif_parse", "error", f"BIFF unreadable: {exc}", rel))
                row["error"] = str(exc)
            result["bifs"].append(row)

        referenced = {os.path.normcase(os.path.basename(rel)) for _m, rel in declared}
        for path in bif_paths:
            if os.path.normcase(os.path.basename(path)) not in referenced:
                issues.append(_issue(
                    "bif_unreferenced", "warning",
                    "data/*.bif is not referenced by chitin.key", _rel(root, path),
                ))

    # ---- TLK ----
    tlk_path = os.path.join(root, "dialog.tlk")
    if os.path.isfile(tlk_path):
        try:
            tlk = parse_tlk(tlk_path)
            result["tlk"] = tlk
            issues.extend(tlk.pop("issues", []))
        except Exception as exc:
            issues.append(_issue("tlk_parse", "error", f"dialog.tlk unreadable: {exc}", "dialog.tlk"))

    # ---- containers ----
    for name, pattern in (("modules", ".rim"), ("rims", ".rim")):
        d = os.path.join(root, name)
        rows = {"count": 0, "bytes": 0, "bad": 0}
        if os.path.isdir(d):
            for fn in sorted(os.listdir(d)):
                path = os.path.join(d, fn)
                if not os.path.isfile(path) or not fn.lower().endswith(pattern):
                    continue
                rows["count"] += 1
                rows["bytes"] += os.path.getsize(path)
                try:
                    with open(path, "rb") as f:
                        magic = f.read(8)
                except OSError as exc:
                    rows["bad"] += 1
                    issues.append(_issue("rim_read", "error", f"unreadable RIM: {exc}", _rel(root, path)))
                    continue
                if magic != b"RIM V1.0":
                    rows["bad"] += 1
                    issues.append(_issue("rim_magic", "error", "not a RIM V1.0 file", _rel(root, path)))
        result["archives"][name] = rows
        if rows["count"] == 0 and name in REQUIRED_DIRS:
            issues.append(_issue(f"{name}_empty", "warning", f"{name}/ contains no {pattern} files", name + "/"))

    patch = os.path.join(root, "patch.erf")
    if os.path.isfile(patch):
        try:
            with open(patch, "rb") as f:
                head = f.read(24)
            if head[:4] not in (b"ERF ", b"MOD ", b"SAV "):
                raise ValueError("not an ERF/MOD container")
            count = struct.unpack_from("<I", head, 0x10)[0]
            off_key = struct.unpack_from("<I", head, 0x14)[0]
            size = os.path.getsize(patch)
            if off_key + count * 24 > size:
                raise ValueError("ERF key table outside file")
            result["archives"]["patch_erf"] = {
                "layout": "kotor-v1", "entry_count": count, "size": size,
            }
        except Exception as exc:
            issues.append(_issue("patch_erf_parse", "error", f"patch.erf unreadable: {exc}", "patch.erf"))

    # ---- expected hashes ----
    for rel, expected in sorted((expect or {}).items()):
        path = os.path.join(root, *rel.split("/"))
        if not os.path.isfile(path):
            issues.append(_issue("expect_missing", "error", f"expected file missing: {rel}", rel))
            continue
        actual = sha256_file(path)
        if actual != expected:
            issues.append(_issue(
                "hash_mismatch", "error",
                f"SHA-256 {actual} != expected {expected}", rel,
            ))

    # ---- dataset identity ----
    dataset = {"scope": "required+data_bif+tlk+patch", "known_reference": False}
    if key and result["tlk"]:
        ref = REFERENCE_DATASETS.get(key["sha256"])
        dataset.update({
            "chitin_key_sha256": key["sha256"],
            "dialog_tlk_sha256": result["tlk"]["sha256"],
            "tlk_string_count": result["tlk"]["string_count"],
            "bif_count": key["bif_count"],
            "key_count": key["key_count"],
            "language": result["tlk"]["language"],
            "language_id": result["tlk"]["language_id"],
        })
        if ref:
            dataset.update({
                "id": ref["id"],
                "known_reference": True,
                "product_version": ref["product_version"],
                "version_source": ref["version_source"],
                "app_id": ref["app_id"],
                "provenance": ref["provenance"],
            })
        else:
            short = hashlib.sha256(
                (key["sha256"] + result["tlk"]["sha256"]).encode("ascii")
            ).hexdigest()[:12]
            dataset.update({
                "id": f"kotor1-unrecognized-{short}",
                "product_version": None,
                "version_source": "no reference dataset fingerprint match; swkotor.exe intentionally not required",
            })
        if ref and result["tlk"]["language_id"] != ref["language_id"]:
            issues.append(_issue(
                "language_mismatch", "warning",
                f"TLK language {result['tlk']['language']} differs from reference {ref['language']}",
            ))
    result["dataset"] = dataset
    result["ok"] = not any(i["severity"] == "error" for i in issues)
    return result


def summarize(report: dict) -> str:
    d = report["dataset"]
    lines = [f"Install: {report['install']['root']}"]
    if report["key"]:
        lines.append(
            f"KEY: {report['key']['key_count']} resources, {report['key']['bif_count']} BIFs, "
            f"stride {report['key']['entry_stride']}"
        )
    if report["tlk"]:
        lines.append(
            f"TLK: {report['tlk']['string_count']} strings, language {report['tlk']['language']} "
            f"({report['tlk']['language_id']})"
        )
    if d.get("id"):
        lines.append(f"Dataset: {d['id']}  product {d.get('product_version')}")
    for name in ("modules", "rims"):
        a = report["archives"].get(name)
        if a:
            lines.append(f"{name}/: {a['count']} RIMs, {a['bytes']} B, bad={a['bad']}")
    if "patch_erf" in report["archives"]:
        p = report["archives"]["patch_erf"]
        lines.append(f"patch.erf: {p['entry_count']} entries, {p['size']} B")
    errors = [i for i in report["issues"] if i["severity"] == "error"]
    warnings = [i for i in report["issues"] if i["severity"] == "warning"]
    lines.append(f"Result: {'VALID' if report['ok'] else 'INVALID'} ({len(errors)} errors, {len(warnings)} warnings)")
    for i in report["issues"]:
        lines.append(f"  [{i['severity']}] {i['code']}: {i['message']}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game-dir", help="install root (default: $KOTOR_GAME_DIR or $KOTOR_INSTALL)")
    ap.add_argument("--device", help="any path inside/above the install; used to auto-detect the root")
    ap.add_argument("--expect", action="append", default=[], metavar="REL=SHA256",
                    help="expected SHA-256 for an install-relative file (repeatable)")
    ap.add_argument("--json", dest="json_out", help="write the full report JSON here (never inside the install)")
    args = ap.parse_args(argv)

    target = args.game_dir or os.environ.get("KOTOR_GAME_DIR") or os.environ.get("KOTOR_INSTALL")
    if target:
        root = os.path.abspath(target)
    elif args.device:
        found = detect_install(args.device)
        root = found["root"]
        if not root:
            print(f"no install detected from {args.device}; searched {found['searched']}", file=sys.stderr)
            return 2
    else:
        ap.error("provide --game-dir/--device or set KOTOR_GAME_DIR/KOTOR_INSTALL")

    expect = {}
    for item in args.expect:
        if "=" not in item:
            ap.error(f"--expect needs REL=SHA256, got {item!r}")
        rel, digest = item.split("=", 1)
        expect[rel.replace("\\", "/")] = digest.lower()

    report = validate_install(root, expect=expect)
    print(summarize(report))
    if args.json_out:
        dest = os.path.abspath(args.json_out)
        install_root = report["install"]["root"]
        if os.path.commonpath([dest, install_root]) == install_root:
            print("refusing to write JSON inside the install", file=sys.stderr)
            return 3
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "w", encoding="utf-8", newline="\n") as f:
            json.dump(report, f, indent=2, sort_keys=True)
            f.write("\n")
    return 0 if report["ok"] else 2


if __name__ == "__main__":
    sys.exit(main())
