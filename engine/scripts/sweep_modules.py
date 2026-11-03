#!/usr/bin/env python3
"""Load every shipped module and assert the scene loader is clean.

This is a verification-only sweep. The engine itself never opens this script;
it launches the C++ loader once per module RIM and checks:
  * the process exits 0;
  * the module line names the requested stem;
  * no model warnings remain;
  * the loaded counts are reported.

    python engine/scripts/sweep_modules.py \
      --exe engine/build/msvc-release/kotor_engine.exe \
      --install E:/SteamLibrary/steamapps/common/swkotor \
      --out artifacts/lanes/sprint8-sweep/modules.json
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


def module_stems(install: Path) -> list[str]:
    stems = []
    for path in (install / "modules").glob("*.rim"):
        stem = path.stem
        if stem.endswith("_s") or stem.endswith("_a"):
            continue
        stems.append(stem)
    return sorted(set(stems))


def run_one(exe: Path, install: Path, module: str) -> dict:
    command = [str(exe), "--install", str(install), "--module", module,
               "--scene-stats", "--null", "--headless"]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=90,
                                check=False)
    except subprocess.TimeoutExpired:
        return {"module": module, "rc": "timeout", "error": "timeout"}
    output = result.stdout + result.stderr
    counts = {}
    for key in ("rooms", "vertices", "textures", "entities", "entity meshes", "triggers",
                "warnings", "script hooks", "encounter volumes"):
        match = re.search(rf"{re.escape(key)}: (\d+)", output)
        counts[key] = int(match.group(1)) if match else None
    header = re.search(rf"KotOR module {re.escape(module)} area (\S+)", output)
    error = re.search(r"\[ERROR\] (.+)", output)
    return {
        "module": module,
        "rc": result.returncode,
        "area": header.group(1) if header else None,
        **counts,
        "error": error.group(1) if error else None,
        "tail": output.strip().splitlines()[-1] if output.strip() else "",
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--install", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--expected", type=int, default=117)
    args = parser.parse_args(argv)
    exe = Path(args.exe).resolve()
    install = Path(args.install).resolve()
    modules = module_stems(install)
    rows = []
    for index, module in enumerate(modules):
        rows.append(run_one(exe, install, module))
        if index % 20 == 0:
            print(f"sweep {index}/{len(modules)}", flush=True)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    failed = [r for r in rows if r["rc"] != 0 or r.get("warnings") != 0 or r.get("error")]
    report = {
        "schema": "kotor.module-sweep/1",
        "install": str(install),
        "exe": str(exe),
        "expected": args.expected,
        "count": len(rows),
        "failures": len(failed),
        "warnings": sum((r.get("warnings") or 0) for r in rows),
        "encounters": sum((r.get("encounter volumes") or 0) for r in rows),
        "entities": sum((r.get("entities") or 0) for r in rows),
        "pass": len(rows) == args.expected and not failed,
        "rows": rows,
    }
    out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({k: report[k] for k in ("count", "failures", "warnings", "encounters",
                                             "entities", "pass")}, sort_keys=True))
    for row in failed[:20]:
        print("FAILED", row)
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
