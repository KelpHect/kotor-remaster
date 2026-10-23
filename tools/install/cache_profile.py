#!/usr/bin/env python3
"""Build and verify an external, content-addressed cache profile for a KotOR
install. Standard library only.

The profile is written *outside* the install and records:

* dataset identity: id, product version, language, scope
* install root (informational; excluded from the profile hash so relocating an
  unmodified install keeps the same dataset identity)
* per-file SHA-256, size and kind for every dataset file
* counts (files, bytes, by kind) and stat-only totals for the whole install
* ``profile_hash``: SHA-256 over the canonical identity payload below

Canonical payload (sorted keys, compact separators)::

    {"schema", "dataset": {id, product_version, language, language_id, scope},
     "counts": {files, bytes, by_kind},
     "files": [{path, size, sha256, kind}, ...]}

No timestamps, mtimes or iteration-order dependence enter the profile, so two
runs over an unchanged install produce byte-identical JSON.

The install is only ever opened for reading. Exit codes: 0 ok, 2 invalid/
mismatch, 3 refused/usage.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys

SCHEMA = "kotor-cache-profile/1"

REQUIRED_NAMES = ("chitin.key", "dialog.tlk", "patch.erf")
INSTALL_MARKERS = ("chitin.key", "swkotor.exe")

# kind is derived from the install-relative POSIX path; mirrors the Sprint 0
# manifest kinds so the two datasets can be reconciled row by row.
def classify(rel: str) -> str:
    p = rel.lower()
    base = p.rsplit("/", 1)[-1]
    if base == "chitin.key":
        return "key"
    if base == "dialog.tlk":
        return "talk_table"
    if base == "patch.erf":
        return "patch_erf"
    if p.startswith("data/") and p.endswith(".bif"):
        return "data_bif"
    if p.startswith("modules/") and p.endswith(".rim"):
        return "module_rim"
    if p.startswith("rims/") and p.endswith(".rim"):
        return "rim"
    if p.startswith("texturepacks/") and p.endswith(".erf"):
        return "texture_pack"
    if p.startswith("movies/") and p.endswith(".bik"):
        return "movie"
    if p.startswith("streamwaves/"):
        return "stream_voice"
    if p.startswith("streammusic/"):
        return "stream_music"
    if p.startswith("streamsounds/"):
        return "stream_sound"
    if p.startswith("lips/"):
        return "lip_sync"
    if p.startswith("launcher/"):
        return "launcher_asset"
    if p.endswith(".exe"):
        return "executable"
    if p.endswith((".dll", ".asi", ".flt", ".m3d")):
        return "library"
    if p.endswith((".ini", ".vdf", ".txt")):
        return "config"
    if p.endswith((".bmp", ".tga", ".tpc")):
        return "bitmap"
    if p.startswith("saves/"):
        return "save"
    if p.startswith("miles/"):
        return "audio_driver"
    return "other"


DATASET_PREFIXES = ("data/", "modules/", "rims/", "texturepacks/")
DATASET_ROOT_FILES = ("chitin.key", "dialog.tlk", "patch.erf")


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class CacheOutputRefused(RuntimeError):
    """Raised when a profile/JSON output would land in an install directory."""


def _norm(path: str) -> str:
    return os.path.normcase(os.path.realpath(os.path.abspath(path)))


def assert_output_allowed(out_path: str, install_root: str | None = None) -> str:
    """Refuse output inside an install, or inside any ancestor that looks like
    one (contains chitin.key or swkotor.exe). Returns the resolved directory."""
    dest = os.path.abspath(out_path)
    target_dir = os.path.dirname(dest)
    if not target_dir:
        raise CacheOutputRefused(f"cannot resolve output directory for {out_path}")
    # Walk ancestors of the intended directory; the install root is one of them
    # when the output is inside the install.
    cur = target_dir
    while True:
        for marker in INSTALL_MARKERS:
            if os.path.isfile(os.path.join(cur, marker)):
                raise CacheOutputRefused(
                    f"refusing to write inside an install directory ({marker} found in {cur})"
                )
        parent = os.path.dirname(cur)
        if parent == cur:
            break
        cur = parent
    if install_root:
        root = _norm(install_root)
        destn = _norm(target_dir)
        if destn == root or destn.startswith(root + os.sep):
            raise CacheOutputRefused(f"refusing to write inside the install root {install_root}")
    return target_dir


def _iter_regular_files(root: str):
    """Yield install-relative POSIX paths of regular files, deterministic order."""
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort()
        rel_dir = os.path.relpath(dirpath, root)
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            try:
                st = os.lstat(path)
            except OSError:
                continue
            if not os.path.isfile(path) or os.path.islink(path):
                continue
            if getattr(st, "st_file_attributes", 0) & 0x400:  # reparse point
                continue
            rel = name if rel_dir == "." else os.path.join(rel_dir, name)
            yield rel.replace("\\", "/"), path, st.st_size


def _in_scope(rel: str, scope: str) -> bool:
    if scope == "all":
        return True
    low = rel.lower()
    if low in DATASET_ROOT_FILES:
        return True
    return any(low.startswith(p) for p in DATASET_PREFIXES)


def canonical_payload(profile: dict) -> dict:
    d = profile["dataset"]
    return {
        "schema": profile["schema"],
        "dataset": {
            "id": d["id"],
            "product_version": d.get("product_version"),
            "language": d.get("language"),
            "language_id": d.get("language_id"),
            "scope": d["scope"],
        },
        "counts": {
            "files": profile["counts"]["files"],
            "bytes": profile["counts"]["bytes"],
            "by_kind": profile["counts"]["by_kind"],
        },
        "files": profile["files"],
    }


def profile_digest(profile: dict) -> str:
    blob = json.dumps(
        canonical_payload(profile), sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()


def build_profile(game_dir: str, scope: str = "dataset", expect_ok: bool = True) -> dict:
    """Validate the install and build a cache profile in memory."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from install_validate import validate_install  # local module; stdlib only

    root = os.path.abspath(game_dir)
    report = validate_install(root)
    if expect_ok and not report["ok"]:
        errors = [i["message"] for i in report["issues"] if i["severity"] == "error"]
        raise ValueError("install failed validation: " + "; ".join(errors[:8]))

    files = []
    by_kind: dict[str, int] = {}
    total = 0
    install_files = 0
    install_bytes = 0
    for rel, path, size in _iter_regular_files(root):
        install_files += 1
        install_bytes += size
        if not _in_scope(rel, scope):
            continue
        digest = sha256_file(path)
        kind = classify(rel)
        files.append({"path": rel, "size": size, "sha256": digest, "kind": kind})
        by_kind[kind] = by_kind.get(kind, 0) + 1
        total += size

    ds = report["dataset"]
    profile = {
        "schema": SCHEMA,
        "dataset": {
            "id": ds.get("id"),
            "game": "Star Wars: Knights of the Old Republic (K1)",
            "product_version": ds.get("product_version"),
            "version_source": ds.get("version_source"),
            "language": ds.get("language"),
            "language_id": ds.get("language_id"),
            "scope": scope,
            "known_reference": ds.get("known_reference", False),
        },
        "install": {
            "root": root.replace("\\", "/"),
            "executable_present": report["install"]["executable_present"],
            "swkotor_exe_required": False,
        },
        "structure": {
            "key_sha256": report["key"]["sha256"] if report["key"] else None,
            "key_count": report["key"]["key_count"] if report["key"] else None,
            "bif_count": report["key"]["bif_count"] if report["key"] else None,
            "tlk_sha256": report["tlk"]["sha256"] if report["tlk"] else None,
            "tlk_string_count": report["tlk"]["string_count"] if report["tlk"] else None,
            "module_rims": report["archives"].get("modules", {}).get("count"),
            "global_rims": report["archives"].get("rims", {}).get("count"),
            "patch_erf_entries": report["archives"].get("patch_erf", {}).get("entry_count"),
        },
        "counts": {
            "files": len(files),
            "bytes": total,
            "by_kind": dict(sorted(by_kind.items())),
            "install_regular_files": install_files,
            "install_bytes": install_bytes,
        },
        "files": files,
    }
    profile["profile_hash"] = profile_digest(profile)
    return profile


def write_profile(profile: dict, out_path: str, install_root: str | None = None) -> str:
    target_dir = assert_output_allowed(out_path, install_root)
    os.makedirs(target_dir, exist_ok=True)
    dest = os.path.abspath(out_path)
    with open(dest, "w", encoding="utf-8", newline="\n") as f:
        json.dump(profile, f, indent=2, sort_keys=True, ensure_ascii=True)
        f.write("\n")
    return dest


def load_profile(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def verify_profile(profile_path: str, game_dir: str | None = None,
                   check_unexpected: bool = True) -> dict:
    """Re-hash the profile's files and compare against the recorded identity."""
    profile = load_profile(profile_path)
    issues: list[dict] = []
    if profile.get("schema") != SCHEMA:
        issues.append({"code": "schema", "severity": "error",
                       "message": f"unsupported schema {profile.get('schema')!r}"})
        return {"ok": False, "profile_hash_match": False, "issues": issues}

    recorded = profile.get("profile_hash")
    actual = profile_digest(profile)
    hash_match = recorded == actual
    if not hash_match:
        issues.append({"code": "profile_hash", "severity": "error",
                       "message": f"profile hash {actual} != recorded {recorded}"})

    root = os.path.abspath(game_dir or profile["install"]["root"])
    checked = 0
    matched = 0
    for row in profile["files"]:
        rel = row["path"]
        path = os.path.join(root, *rel.split("/"))
        if not os.path.isfile(path):
            issues.append({"code": "missing", "severity": "error",
                           "message": "file missing from install", "path": rel})
            continue
        size = os.path.getsize(path)
        if size != row["size"]:
            issues.append({"code": "size_mismatch", "severity": "error",
                           "message": f"size {size} != recorded {row['size']}", "path": rel})
            continue
        digest = sha256_file(path)
        checked += 1
        if digest != row["sha256"]:
            issues.append({"code": "hash_mismatch", "severity": "error",
                           "message": f"SHA-256 {digest} != recorded {row['sha256']}", "path": rel})
        else:
            matched += 1

    if check_unexpected:
        recorded_paths = {row["path"] for row in profile["files"]}
        for rel, _path, _size in _iter_regular_files(root):
            if _in_scope(rel, profile["dataset"]["scope"]) and rel not in recorded_paths:
                issues.append({"code": "unexpected", "severity": "warning",
                               "message": "dataset file not present in profile", "path": rel})

    return {
        "ok": not any(i["severity"] == "error" for i in issues),
        "profile_hash_match": hash_match,
        "profile_hash": actual,
        "files_recorded": len(profile["files"]),
        "files_checked": checked,
        "files_matched": matched,
        "install_root": root,
        "issues": issues,
    }


def summarize_profile(profile: dict) -> str:
    d = profile["dataset"]
    c = profile["counts"]
    lines = [
        f"Dataset: {d['id']}",
        f"Product version: {d.get('product_version')} ({d.get('version_source')})",
        f"Language: {d.get('language')} ({d.get('language_id')})  scope={d['scope']}",
        f"Root: {profile['install']['root']}",
        f"Hashed: {c['files']} files, {c['bytes']} B  by_kind={c['by_kind']}",
        f"Install stat totals: {c['install_regular_files']} files, {c['install_bytes']} B",
        f"profile_hash: {profile['profile_hash']}",
    ]
    return "\n".join(lines)


def build_cli(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="cache_profile.py build")
    ap.add_argument("--game-dir", help="install root (default: $KOTOR_GAME_DIR or $KOTOR_INSTALL)")
    ap.add_argument("--out", required=True, help="profile JSON path (must be outside the install)")
    ap.add_argument("--scope", choices=("dataset", "all"), default="dataset",
                    help="dataset = keys/BIFs/RIMs/TLK/patch/texture packs (default); all = every regular file")
    ap.add_argument("--allow-invalid", action="store_true", help="profile even if validation fails")
    args = ap.parse_args(argv)

    game_dir = args.game_dir or os.environ.get("KOTOR_GAME_DIR") or os.environ.get("KOTOR_INSTALL")
    if not game_dir:
        ap.error("provide --game-dir or set KOTOR_GAME_DIR/KOTOR_INSTALL")
    try:
        assert_output_allowed(args.out, game_dir)
        profile = build_profile(game_dir, scope=args.scope, expect_ok=not args.allow_invalid)
        dest = write_profile(profile, args.out, install_root=game_dir)
    except CacheOutputRefused as exc:
        print(f"REFUSED: {exc}", file=sys.stderr)
        return 3
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    print(summarize_profile(profile))
    print(f"Wrote {dest} ({os.path.getsize(dest)} B, sha256 {sha256_file(dest)})")
    return 0


def verify_cli(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="cache_profile.py verify")
    ap.add_argument("--profile", required=True)
    ap.add_argument("--game-dir", help="verify a relocated install against the profile")
    ap.add_argument("--no-unexpected", action="store_true")
    args = ap.parse_args(argv)
    try:
        result = verify_profile(args.profile, args.game_dir, check_unexpected=not args.no_unexpected)
    except (OSError, ValueError, KeyError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    print(f"root: {result['install_root']}")
    print(f"profile_hash: {result['profile_hash']} (match={result['profile_hash_match']})")
    print(f"files: {result['files_matched']}/{result['files_recorded']} matched, "
          f"{result['files_checked']} hashed")
    for i in result["issues"]:
        print(f"  [{i['severity']}] {i['code']}: {i.get('path', '')} {i['message']}")
    print("RESULT:", "OK" if result["ok"] else "MISMATCH")
    return 0 if result["ok"] else 2


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] not in ("build", "verify"):
        print("usage: cache_profile.py {build,verify} ...", file=sys.stderr)
        return 3
    cmd, rest = argv[0], argv[1:]
    return build_cli(rest) if cmd == "build" else verify_cli(rest)


if __name__ == "__main__":
    sys.exit(main())
