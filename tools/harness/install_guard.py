#!/usr/bin/env python3
"""Watch a game install's config files and restore them if a harness modifies them.

KotOR can rewrite `swkotor.ini` in the install even when the harness launches a
copy of the executable from a separate profile. This guard snapshots every
top-level config file recorded in a manifest JSONL, verifies it against the
manifest hash, then restores the exact bytes and mtime if the file changes.

Usage:
  python tools/harness/install_guard.py --install "<KOTOR_INSTALL>" \
      --manifest artifacts/lanes/01-manifest/manifest.jsonl --minutes 60

The guard is read-only until a change is detected. It never writes tracked files.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
from pathlib import Path

CONFIG_SUFFIXES = (".ini", ".cfg", ".conf")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_configs(manifest: Path) -> list[dict]:
    out = []
    with manifest.open(encoding="utf-8") as f:
        for line in f:
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                continue
            path = str(e.get("path", ""))
            if e.get("kind") == "config" or path.lower().endswith(CONFIG_SUFFIXES):
                if "/" not in path and "\\" not in path:
                    out.append(e)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--minutes", type=float, default=60.0)
    ap.add_argument("--log", default=None)
    args = ap.parse_args()

    install = Path(args.install)
    manifest = Path(args.manifest)
    log_path = Path(args.log) if args.log else None
    logf = log_path.open("a", encoding="utf-8") if log_path else sys.stdout

    def log(msg: str) -> None:
        line = f"{time.strftime('%Y-%m-%dT%H:%M:%S')} {msg}"
        logf.write(line + "\n")
        logf.flush()
        if logf is not sys.stdout:
            print(line, flush=True)

    baseline: dict[str, tuple[bytes, int]] = {}
    for e in load_configs(manifest):
        p = install / e["path"]
        if not p.is_file():
            log(f"skip missing config {p}")
            continue
        data = p.read_bytes()
        if sha256(data) != e.get("sha256"):
            log(f"WARNING {p} does not match manifest at start; not guarding it")
            continue
        baseline[str(p)] = (data, int(e.get("mtime_ns", 0)))
        log(f"guarding {p} ({len(data)} B)")

    if not baseline:
        log("no config files to guard")
        return 0

    deadline = time.monotonic() + args.minutes * 60.0
    restores = 0
    while time.monotonic() < deadline:
        for path, (data, mtime_ns) in baseline.items():
            try:
                current = Path(path).read_bytes()
            except OSError as exc:
                log(f"read failed {path}: {exc}")
                continue
            if current != data:
                try:
                    Path(path).write_bytes(data)
                    if mtime_ns:
                        os.utime(path, ns=(mtime_ns, mtime_ns))
                    restores += 1
                    log(f"RESTORED {path} ({len(current)} -> {len(data)} B)")
                except OSError as exc:
                    log(f"restore failed {path}: {exc}")
        time.sleep(args.interval)
    log(f"guard finished; restores={restores}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
