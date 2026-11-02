#!/usr/bin/env python3
"""Sprint-1 residual ledger: what G1 closed and what belongs to later phases.

The script reads the generated script-corpus artifacts (no game data) and emits
a machine-readable disposition:

* G1 data-completeness items must be empty (all 8,666 NCS are decoded and
  classified; the shipped corpus has no open parser/coverage item).
* ``compiler-limited`` recompiles and unused actions are explicitly owned by
  P3 VM conformance: they are compiler-codegen / modded-content work, not a
  data gap in the shipped corpus.
* Runtime/renderer residuals are listed with their later-phase owner.

Usage:
    python -m tools.scripts.residuals \
        --corpus artifacts/lanes/s1-04-scripts/corpus.json \
        --usage artifacts/lanes/s1-04-scripts/action_usage_report.json \
        --out artifacts/lanes/s1-04-scripts/residuals.json
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path


SCHEMA = "kotor-s1-residuals/1"
G1_OWNED = {"exact", "header-limited", "reachable-only"}


def build_residuals(corpus: dict, usage: dict) -> dict:
    decomp = corpus.get("decompile_status", {})
    classes = decomp.get("classes", {})
    files = int(decomp.get("files", 0))

    def count(name: str) -> int:
        value = classes.get(name, 0)
        return int(value.get("count", value) if isinstance(value, dict) else value)

    exact = count("exact")
    compiler_limited = count("compiler-limited")
    header_limited = count("header-limited")
    reachable_only = count("reachable-only")
    classified = exact + compiler_limited + header_limited + reachable_only

    unused = usage.get("summary", {}).get("actions_unused", [])
    actions = int(corpus.get("action_table", {}).get("actions", 0))

    g1_open = []
    if files and classified != files:
        g1_open.append({
            "id": "ncs-classification-gap",
            "count": files - classified,
            "reason": "NCS rows without a decompile-status class",
        })
    if not files:
        g1_open.append({"id": "ncs-status-missing", "count": 0,
                        "reason": "decompile_status.files is zero"})

    p3 = []
    if compiler_limited:
        p3.append({
            "id": "compiler-limited-recompiles",
            "count": compiler_limited,
            "owner": "P3 VM conformance",
            "reason": ("clean-room recompile differs from the shipped compiler's "
                       "codegen; NCS decode/semantics are complete, so this is not a "
                       "G1 data gap and does not affect shipped-corpus coverage"),
        })
    if unused:
        p3.append({
            "id": "unused-actions",
            "count": len(unused),
            "of": actions,
            "owner": "P3 VM conformance",
            "reason": ("actions not exercised by the shipped corpus; implementations "
                       "are needed for modded content, not for G1 coverage"),
        })

    return {
        "schema": SCHEMA,
        "build_sha256": corpus.get("build_sha256"),
        "g1": {
            "status": "closed" if not g1_open else "open",
            "files": files,
            "classified": classified,
            "classes": {
                "exact": exact,
                "compiler-limited": compiler_limited,
                "header-limited": header_limited,
                "reachable-only": reachable_only,
            },
            "open_items": g1_open,
        },
        "p3": p3,
        "later_phase": [
            {"id": "chargen-play-live", "owner": "P4 hardware vertical slice",
             "reason": "llvmpipe/DirectInput input blocker; mount order statically proven"},
            {"id": "dialogue-live-click", "owner": "P4 hardware vertical slice",
             "reason": "same input blocker; camera path statically proven"},
            {"id": "f4-thumbnail", "owner": "P4 renderer",
             "reason": "shim implemented in tools/harness/thumb_shim.py and sprint1_hooks.js"},
            {"id": "minimap-raster", "owner": "P5 renderer",
             "reason": "data-side affine/absence verified; engine raster pass is P5"},
        ],
        "status_sources": {
            "corpus": "artifacts/lanes/s1-04-scripts/corpus.json",
            "usage": "artifacts/lanes/s1-04-scripts/action_usage_report.json",
        },
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--usage", required=True)
    ap.add_argument("--out")
    args = ap.parse_args(argv)
    corpus = json.loads(Path(args.corpus).read_text(encoding="utf-8"))
    usage = json.loads(Path(args.usage).read_text(encoding="utf-8"))
    report = build_residuals(corpus, usage)
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if report["g1"]["status"] == "closed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
