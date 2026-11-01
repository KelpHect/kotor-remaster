#!/usr/bin/env python3
"""Map compiler CLI (lane s1-03): compile + validate all 117 module pairs.

Read-only against the install.  Verifies the swkotor.exe SHA-256 sentinel
first and stops on drift.  Writes per-module JSON reports and a summary to
the lane artifacts dir (gitignored).  Exit code 1 if any area hard-fails.

Usage (Git Bash):
  python tools/mapcompile/compile.py [--game-dir E:/SteamLibrary/steamapps/common/swkotor]
                                     [--out artifacts/lanes/s1-03-mapcompile]
                                     [--module end_m01aa ...] [--quiet]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
_TOOLS = os.path.join(_ROOT, "tools")
for _p in (_ROOT, _TOOLS):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from mapcompile import BUILD_SHA256, PACKAGE_VERSION  # noqa: E402
from mapcompile import constants as C  # noqa: E402
from mapcompile.loader import GameStore, LoadError, load_module  # noqa: E402
from mapcompile.validator import validate_module  # noqa: E402


def verify_install(game_dir: str) -> str:
    exe = os.path.join(game_dir, "swkotor.exe")
    h = hashlib.sha256()
    with open(exe, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    digest = h.hexdigest()
    if digest != BUILD_SHA256:
        raise SystemExit(f"INSTALL DRIFT: swkotor.exe sha256 {digest} != {BUILD_SHA256}; stop")
    return digest


def compile_one(store, module):
    md = load_module(store, module)
    report = validate_module(md)
    report["build_sha256"] = BUILD_SHA256
    report["compiler_version"] = PACKAGE_VERSION
    report["sources"] = md.sources
    return report


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="KotOR map compiler + validator (s1-03)")
    ap.add_argument("--game-dir", default=os.environ.get("KOTOR_GAME_DIR", ""))
    ap.add_argument("--out", default=os.path.join(
        _ROOT, "artifacts", "lanes", "s1-03-mapcompile"))
    ap.add_argument("--module", action="append", default=None,
                    help="restrict to these module names (repeatable)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if not args.game_dir:
        ap.error("--game-dir or KOTOR_GAME_DIR is required")
    digest = verify_install(args.game_dir)
    if not args.quiet:
        print(f"install sentinel OK: swkotor.exe sha256 {digest}")

    store = GameStore(args.game_dir)
    modules = args.module if args.module else store.modules()
    if not args.quiet:
        print(f"modules: {len(modules)} (expected 117)")

    os.makedirs(os.path.join(args.out, "reports"), exist_ok=True)
    reports = []
    t0 = time.time()
    for i, module in enumerate(modules):
        try:
            rep = compile_one(store, module)
        except Exception as exc:  # load/parse failure is a hard report entry
            rep = {"module": module, "area": None, "status": "hard_failed",
                   "hard_failures": 1, "triage": 0,
                   "entries": [{"rule": C.R_LOAD, "code": C.R_LOAD, "severity": "hard",
                                "message": f"{type(exc).__name__}: {exc}", "refs": []}],
                   "stats": {}, "build_sha256": BUILD_SHA256,
                   "compiler_version": PACKAGE_VERSION}
        reports.append(rep)
        path = os.path.join(args.out, "reports", f"{module}.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(rep, f, indent=1, sort_keys=True)
        if not args.quiet:
            codes = sorted({e["code"] for e in rep["entries"]})
            print(f"[{i + 1:3d}/{len(modules)}] {module:14s} area={str(rep['area']):10s} "
                  f"{rep['status']:12s} hard={rep['hard_failures']} triage={rep['triage']} "
                  f"codes={','.join(codes) if codes else '-'}")

    clean = [r for r in reports if r["status"] == "clean"]
    triaged = [r for r in reports if r["status"] == "triaged"]
    failed = [r for r in reports if r["status"] == "hard_failed"]
    code_counts = {}
    for r in reports:
        for e in r["entries"]:
            code_counts[e["code"]] = code_counts.get(e["code"], 0) + 1
    summary = {
        "build_sha256": BUILD_SHA256,
        "compiler_version": PACKAGE_VERSION,
        "game_dir": args.game_dir,
        "modules_total": len(reports),
        "areas_clean": len(clean),
        "areas_triaged": len(triaged),
        "areas_hard_failed": len(failed),
        "clean_modules": [r["module"] for r in clean],
        "triaged_modules": {r["module"]: sorted({e["code"] for e in r["entries"]})
                            for r in triaged},
        "hard_failed_modules": {r["module"]: sorted({e["code"] for e in r["entries"]})
                                for r in failed},
        "code_counts": code_counts,
        "elapsed_s": round(time.time() - t0, 2),
        "hard_fail_exit": 1 if failed else 0,
    }
    with open(os.path.join(args.out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1, sort_keys=True)
    print(f"\nclean={len(clean)} triaged={len(triaged)} hard_failed={len(failed)} "
          f"of {len(reports)} modules in {summary['elapsed_s']}s")
    print(f"summary -> {os.path.join(args.out, 'summary.json')}")
    if code_counts:
        print("codes: " + ", ".join(f"{k}={v}" for k, v in sorted(code_counts.items())))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
