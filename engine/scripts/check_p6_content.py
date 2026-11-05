#!/usr/bin/env python3
"""P6 all-area content checklist for the 117 shipped KotOR 1 modules.

Runs ``kotor_engine --content-stats`` for every module, compares the
machine-readable scene counts against a committed baseline, and asserts that
every area plus the aggregate content categories are covered. The gate is
verification-only: it never writes into the game install.

    python engine/scripts/check_p6_content.py \
      --exe engine/build/msvc-release/kotor_engine.exe \
      --install E:/SteamLibrary/steamapps/common/swkotor \
      --baseline engine/tests/fixtures/p6_content_baseline.json \
      --out docs/evidence/s13/p6-content.json

Use ``--write-baseline`` after an intentional engine/data change.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

KOTOR1_SHA256 = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6"
ANSI = re.compile(r"\x1b\[[0-9;]*m")
NUMERIC = (
    "rooms", "vertices", "indices", "batches", "textures",
    "collision_vertices", "collision_facets", "entities", "doors",
    "placeables", "creatures", "waypoints", "stores", "encounter_entities",
    "sound_entities", "entity_meshes", "triggers", "encounter_volumes",
    "cameras", "sounds", "script_hooks", "scripted_entities",
    "scene_scripts", "script_runs", "warnings",
)
COVERAGE = (
    "rooms", "vertices", "indices", "batches", "textures", "entities",
    "doors", "placeables", "creatures", "waypoints", "stores",
    "entity_meshes", "triggers", "encounter_volumes", "cameras", "sounds",
    "script_hooks", "scene_scripts",
)


def module_stems(install: Path) -> list[str]:
    stems = []
    for path in (install / "modules").glob("*.rim"):
        stem = path.stem
        if stem.endswith("_s") or stem.endswith("_a") or stem.endswith("_adx"):
            continue
        stems.append(stem)
    return sorted(set(stems))


def install_sha(install: Path) -> str:
    exe = install / "swkotor.exe"
    digest = hashlib.sha256()
    with exe.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_one(exe: Path, install: Path, module: str) -> dict:
    command = [str(exe), "--install", str(install), "--module", module,
               "--content-stats", "--null", "--headless"]
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=120, check=False)
    except subprocess.TimeoutExpired:
        return {"module": module, "error": "timeout", "rc": "timeout"}
    output = ANSI.sub("", result.stdout + result.stderr)
    for line in output.splitlines():
        marker = line.find("content-stats: ")
        if marker < 0:
            continue
        try:
            row = json.loads(line[marker + len("content-stats: "):])
        except json.JSONDecodeError as exc:
            return {"module": module, "error": f"json: {exc}", "rc": result.returncode}
        row["rc"] = result.returncode
        return row
    match = re.search(r"\[ERROR\] (.+)", output)
    return {"module": module, "rc": result.returncode,
            "error": match.group(1) if match else "no content-stats line"}


def aggregate(rows: dict[str, dict]) -> dict:
    totals = {key: 0 for key in NUMERIC}
    for row in rows.values():
        for key in NUMERIC:
            totals[key] += int(row.get(key) or 0)
    totals["areas"] = len({row.get("area") for row in rows.values() if row.get("area")})
    totals["modules"] = len(rows)
    return totals


def validate(rows: dict[str, dict], baseline: dict, install: Path) -> list[str]:
    failures = []
    expected_sha = baseline.get("install_sha256")
    actual_sha = install_sha(install)
    if expected_sha and actual_sha != expected_sha:
        failures.append(f"install sha256 {actual_sha} != baseline {expected_sha}")
    if len(rows) != baseline["count"]:
        failures.append(f"module count {len(rows)} != {baseline['count']}")
    expected = baseline["modules"]
    for module in sorted(set(expected) | set(rows)):
        if module not in rows:
            failures.append(f"{module}: missing from sweep")
            continue
        if module not in expected:
            failures.append(f"{module}: unexpected module")
            continue
        row = rows[module]
        if row.get("rc") != 0 or row.get("error"):
            failures.append(f"{module}: {row.get('error') or row.get('rc')}")
            continue
        if not row.get("area"):
            failures.append(f"{module}: empty area")
        for key in ("rooms", "vertices", "textures", "entities"):
            if int(row.get(key) or 0) < 1:
                failures.append(f"{module}: {key}=0")
        if int(row.get("warnings") or 0) != 0:
            failures.append(f"{module}: warnings={row.get('warnings')}")
        for key in NUMERIC:
            if int(row.get(key) or 0) != int(expected[module].get(key) or 0):
                failures.append(
                    f"{module}: {key} {row.get(key)} != {expected[module].get(key)}")
    totals = aggregate(rows)
    for key, value in baseline["aggregate"].items():
        if totals.get(key) != value:
            failures.append(f"aggregate {key} {totals.get(key)} != {value}")
    for key in COVERAGE:
        if int(totals.get(key) or 0) < 1:
            failures.append(f"coverage {key}=0")
    if totals["areas"] != baseline["aggregate"]["areas"]:
        failures.append(
            f"unique areas {totals['areas']} != {baseline['aggregate']['areas']}")
    return failures


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--install", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--write-baseline", action="store_true")
    parser.add_argument("--accept-baseline-change", action="store_true",
                        help="required to overwrite an existing baseline")
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args(argv)
    exe = Path(args.exe).resolve()
    install = Path(args.install).resolve()
    baseline_path = Path(args.baseline)
    out = Path(args.out)
    if args.write_baseline and baseline_path.exists() and not args.accept_baseline_change:
        print(f"refusing to overwrite {baseline_path} without "
              "--accept-baseline-change", file=sys.stderr)
        return 2
    modules = module_stems(install)
    if not modules:
        print(f"no modules under {install / 'modules'}", file=sys.stderr)
        return 1
    print(f"p6 content: {len(modules)} modules from {install}", flush=True)
    rows: dict[str, dict] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {pool.submit(run_one, exe, install, module): module
                   for module in modules}
        for done, future in enumerate(concurrent.futures.as_completed(futures), 1):
            module = futures[future]
            rows[module] = future.result()
            if done % 20 == 0 or done == len(modules):
                print(f"p6 content: {done}/{len(modules)}", flush=True)
    rows = {module: rows[module] for module in sorted(rows)}
    totals = aggregate(rows)
    report = {
        "schema": "kotor.p6-content/1",
        "install": str(install),
        "install_sha256": install_sha(install),
        "exe": str(exe),
        "count": len(rows),
        "aggregate": totals,
        "modules": rows,
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    if args.write_baseline:
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        baseline_document = {
            key: report[key] for key in
            ("schema", "install_sha256", "count", "aggregate", "modules")
        }
        baseline_path.write_text(
            json.dumps(baseline_document, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                       encoding="utf-8")
        print(json.dumps(totals, sort_keys=True))
        return 0 if totals["warnings"] == 0 and totals["modules"] == len(modules) else 1
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    if baseline.get("schema") != "kotor.p6-content/1":
        print("baseline schema mismatch", file=sys.stderr)
        return 2
    failures = validate(rows, baseline, install)
    report["baseline"] = str(baseline_path)
    report["failures"] = failures
    report["pass"] = not failures
    out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                   encoding="utf-8")
    print(json.dumps({key: totals[key] for key in
                      ("modules", "areas", "rooms", "vertices", "entities",
                       "entity_meshes", "triggers", "encounter_volumes",
                       "cameras", "sounds", "script_hooks", "warnings")},
                     sort_keys=True))
    for message in failures:
        print("FAILED", message)
    print("p6 content: PASS" if not failures else "p6 content: FAIL")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
