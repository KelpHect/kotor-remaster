#!/usr/bin/env python3
"""Read-only sentinel for the KotOR install used by the Sprint 6 gates.

The engine opens the install read-only. This script snapshots every regular
file (path, size, mtime, SHA-256) and later proves that no file was added,
removed or modified. It also reports drift against the Sprint 0 manifest; that
drift is informational because the original game may have updated its own
settings before the sprint started.

    python engine/scripts/check_install_readonly.py \
      --install E:/SteamLibrary/steamapps/common/swkotor \
      --manifest artifacts/lanes/01-manifest/manifest.jsonl \
      --baseline artifacts/lanes/s6-07-install/pre.json \
      --out artifacts/lanes/s6-07-install/post.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def snapshot(install: Path) -> dict[str, dict]:
    files: dict[str, dict] = {}
    for root, _dirs, names in os.walk(install):
        for name in names:
            path = Path(root) / name
            try:
                stat = path.stat(follow_symlinks=False)
            except OSError:
                continue
            rel = path.relative_to(install).as_posix()
            files[rel] = {
                "size": stat.st_size,
                "mtime_ns": stat.st_mtime_ns,
                "sha256": sha256_file(path),
            }
    return files


def load_manifest(path: Path | None) -> dict[str, dict]:
    if path is None or not path.exists():
        return {}
    rows: dict[str, dict] = {}
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            row = json.loads(line)
            rows[row["path"]] = {
                "size": row["size_bytes"],
                "mtime_ns": row["mtime_ns"],
                "sha256": row["sha256"],
            }
    return rows


def diff(before: dict[str, dict], after: dict[str, dict]) -> dict:
    missing = sorted(set(before) - set(after))
    extra = sorted(set(after) - set(before))
    changed = []
    for rel in sorted(set(before) & set(after)):
        if before[rel] != after[rel]:
            changed.append({"path": rel, "before": before[rel], "after": after[rel]})
    return {
        "missing": missing[:50],
        "extra": extra[:50],
        "changed": changed[:50],
        "missing_count": len(missing),
        "extra_count": len(extra),
        "changed_count": len(changed),
        "unchanged": not missing and not extra and not changed,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", required=True)
    parser.add_argument("--manifest")
    parser.add_argument("--baseline")
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    install = Path(args.install)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    current = snapshot(install)
    report: dict = {
        "schema": "kotor.install-sentinel/2",
        "install": str(install),
        "files": len(current),
        "bytes": sum(item["size"] for item in current.values()),
        "manifest": args.manifest,
    }
    if args.baseline and Path(args.baseline).exists():
        before = json.loads(Path(args.baseline).read_text(encoding="utf-8"))
        report["baseline"] = args.baseline
        report["session"] = diff(before.get("files", {}), current)
        result = report["session"]["unchanged"]
    else:
        if args.baseline:
            Path(args.baseline).parent.mkdir(parents=True, exist_ok=True)
            Path(args.baseline).write_text(
                json.dumps({"schema": "kotor.install-sentinel/2", "install": str(install),
                            "files": current}, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
            report["baseline_written"] = args.baseline
        result = True
    manifest = load_manifest(Path(args.manifest) if args.manifest else None)
    if manifest:
        report["against_manifest"] = diff(manifest, current)
    out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({
        "files": report["files"],
        "session": report.get("session"),
        "manifest_drift": report.get("against_manifest", {}).get("changed_count"),
        "pass": result,
    }, sort_keys=True))
    return 0 if result else 1


if __name__ == "__main__":
    raise SystemExit(main())
