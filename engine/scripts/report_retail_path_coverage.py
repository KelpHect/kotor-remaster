#!/usr/bin/env python3
"""Report which shipped modules have a recorded retail-vs-engine trace.

This is an informational coverage report, not a pass/fail gate: the recorded
retail corpus is smaller than the 117-module content set. It makes the
remaining quest/cutscene-recording gap explicit and reproducible.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def module_stems(install: Path) -> list[str]:
    modules = []
    for path in (install / "modules").glob("*.rim"):
        stem = path.stem
        if stem.lower().endswith(("_s", "_a", "_adx")):
            continue
        modules.append(stem)
    return sorted(set(modules))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--install", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    covered = sorted({trace["module"] for trace in manifest["traces"]})
    modules = module_stems(Path(args.install))
    covered_set, module_set = set(covered), set(modules)
    missing = sorted(module_set - covered_set)
    report = {
        "schema": "kotor.retail-path-coverage/1",
        "manifest": args.manifest,
        "modules": len(modules),
        "covered_modules": covered,
        "covered_count": len(covered),
        "missing_modules": missing,
        "missing_count": len(missing),
        "complete": not missing,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                   encoding="utf-8")
    print(json.dumps({key: report[key] for key in
                      ("modules", "covered_count", "missing_count", "complete")},
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
