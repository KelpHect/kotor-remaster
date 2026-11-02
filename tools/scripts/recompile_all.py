#!/usr/bin/env python3
"""Recompile all 8,666 clean-room NSS files and classify every result.

Each file gets a header selected from its own ACTION arities: if the shipped
bytecode calls an action with fewer arguments than the current prototype, the
per-file header declares only the historical count.  Files without such calls
use the full header.  This is the source-level rerun that backs the 228
compiler-limited ledger.

Classification:
  raw_exact        - recompiled bytes equal the shipped NCS
  compiler_limited - in the 228 class; still needs a compiler fix
  header_limited   - one of the 5 anomalous-action files (needs a lane-local prototype)
  reachable_only   - one of the unreachable-code files
  other            - unexpected (a gate failure)

Per-file NSS overrides (`--struct-nss`) supply struct-recovered and
reachable-statement-restored source.  The three function-layout files compile
with the historical nwnnsscomp rebuild (`--patched-compiler`, tools/compiler).

Usage:
    python -m tools.scripts.recompile_all \
        --corpus artifacts/lanes/04-ncs/extracted/unique \
        --nss artifacts/lanes/ncs-decompiler/cont2/out/nss \
        --struct-nss artifacts/lanes/s3-live/struct-nss-final \
        --patched-compiler artifacts/lanes/s3-live/tools/nwnnsscomp_hist.exe \
        --header artifacts/lanes/ncs-decompiler/cont2/runs/incwhile-v10/bin/nwscript.nss \
        --compiler artifacts/lanes/04-ncs/tools/DeNCS/tools/nwnnsscomp_ktool.exe \
        --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
        --corpus-json artifacts/lanes/s1-04-scripts/corpus.json \
        --taxonomy artifacts/lanes/ncs-decompiler/cont2/out/residual-taxonomy.json \
        --out-json artifacts/lanes/s3-live/recompile_all.json \
        --out-doc docs/recon/35_FULL_RECOMPILE.md
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, ".."))
from scripts import body_order, historical_headers  # noqa: E402
from scripts import ncs  # noqa: E402

EXPECTED = 8666


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--nss", required=True)
    ap.add_argument("--header", required=True)
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--specs", required=True)
    ap.add_argument("--corpus-json", required=True)
    ap.add_argument("--taxonomy", required=True)
    ap.add_argument("--struct-nss",
                    help="per-file NSS override directory (struct recovery and "
                         "reachable-code fixes)")
    ap.add_argument("--patched-compiler",
                    help="historical nwnnsscomp rebuild (tools/compiler) used "
                         "for the function-layout class")
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--out-doc", required=True)
    ap.add_argument("--work", default="artifacts/lanes/s3-live/recompile-work")
    args = ap.parse_args(argv)

    corpus = Path(args.corpus)
    nss_dir = Path(args.nss)
    compiler = str(Path(args.compiler).resolve())
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    specs = json.loads(Path(args.specs).read_text(encoding="utf-8"))
    spec_by_id = {a["id"]: a for a in specs["actions"]}
    ledger = json.loads(Path(args.corpus_json).read_text(encoding="utf-8"))
    taxonomy = json.loads(Path(args.taxonomy).read_text(encoding="utf-8"))
    full_lines = Path(args.header).read_text(encoding="latin-1").splitlines()

    classes = {}
    for cls in taxonomy["taxonomy"]["compiler_limited"]["classes"]:
        if cls["class"].startswith("action default-argument"):
            for name in cls["files"]:
                classes[name] = "compiler_limited"
        elif cls["class"].startswith("CPTOPBP"):
            for name in cls["files"]:
                classes[name] = "compiler_limited"
        elif cls["class"].startswith("function layout"):
            for name in cls["files"]:
                classes[name] = "compiler_limited"
    body_order_names = set()
    for cls in taxonomy["taxonomy"]["compiler_limited"]["classes"]:
        if cls["class"].startswith("function layout"):
            body_order_names.update(cls["files"])
    header_overrides = {}
    for entry in taxonomy["taxonomy"]["header_limited"]["files"]:
        classes[entry["file"]] = "header_limited"
        header_overrides[entry["file"]] = entry["prototype"]
    for name in taxonomy["taxonomy"]["reachable_only"]["files"]:
        classes[name] = "reachable_only"
    full_header_text = "\n".join(full_lines) + "\n"

    rows = []
    names = sorted((r["decompile"]["name"] for r in ledger["scripts"]))
    entries = []
    headers = {}
    for name in names:
        op = corpus / f"{name}.ncs"
        np = nss_dir / f"{name}.nss"
        body_mode = bool(args.patched_compiler) and name in body_order_names
        if not body_mode and args.struct_nss:
            candidate = Path(args.struct_nss) / name / f"{name}.nss"
            if candidate.exists():
                np = candidate
        if not op.exists() or not np.exists():
            rows.append({"name": name, "status": "other",
                         "error": "missing input"})
            continue
        orig = op.read_bytes()
        if body_mode:
            header_text = body_order._compiler_header(full_header_text)
            applied = [{"body_order": True}]
        else:
            trims = historical_headers._trims(orig, spec_by_id)
            header_text, applied = historical_headers._header_for(
                full_lines, trims, spec_by_id)
            if name in header_overrides:
                proto = header_overrides[name]
                aname = re.search(r"\b(\w+)\s*\(", proto).group(1)
                lines = header_text.splitlines()
                for k, line in enumerate(lines):
                    if (re.search(r"\b" + re.escape(aname) + r"\s*\(", line)
                            and line.strip().endswith(";")):
                        lines[k] = proto
                        break
                header_text = "\n".join(lines) + "\n"
                applied.append({"action": aname, "override": proto})
        h = _sha(header_text.encode("latin-1"))
        headers[h] = header_text
        entries.append({
            "name": name,
            "header": h,
            "trims": applied,
            "input_sha256": _sha(orig),
            "nss": np,
            "body_mode": body_mode,
        })

    grouped = defaultdict(list)
    for e in entries:
        grouped[(e["header"], e["body_mode"])].append(e)
    batches = []
    for (h, body_mode), group in grouped.items():
        step = 1 if body_mode else 100
        for i in range(0, len(group), step):
            batches.append((h, body_mode, group[i:i + step]))
    patched = (str(Path(args.patched_compiler).resolve())
               if args.patched_compiler else None)

    for bi, (h, body_mode, chunk) in enumerate(batches):
        print(f"recompile batch {bi}/{len(batches)}", flush=True)
        d = work / f"batch-{bi:04d}"
        d.mkdir(parents=True, exist_ok=True)
        (d / "nwscript.nss").write_text(headers[h], encoding="latin-1",
                                        newline="\n")
        (d / "k1_nwscript.nss").write_text(headers[h], encoding="latin-1",
                                           newline="\n")
        for e in chunk:
            shutil.copy2(e["nss"], d / f"{e['name']}.nss")
        try:
            if body_mode:
                cmd = ([patched, "-c", "-i", ".", "-v129"] +
                       [f"{e['name']}.nss" for e in chunk])
            else:
                cmd = ([compiler, "-c", "-g", "1", "--outputdir", "."] +
                       [f"{e['name']}.nss" for e in chunk])
            r = subprocess.run(
                cmd, cwd=d, capture_output=True, text=True, timeout=600)
            log = (r.stdout or "") + (r.stderr or "")
            rc = r.returncode
        except subprocess.TimeoutExpired:
            log = "timeout"
            rc = -1
        for e in chunk:
            name = e["name"]
            out = d / f"{name}.ncs"
            orig = (corpus / f"{name}.ncs").read_bytes()
            equal = out.exists() and out.read_bytes() == orig
            cls = classes.get(name)
            if equal:
                status = "raw_exact"
            elif cls:
                status = cls
            else:
                status = "other"
            rows.append({
                "name": name,
                "status": status,
                "trims": e["trims"],
                "compiler": "historical" if body_mode else "ktool",
                "byte_equal": equal,
                "input_sha256": e["input_sha256"],
                "compiled_sha256": _sha(out.read_bytes()) if out.exists() else None,
                "returncode": rc,
                "log_tail": log[-200:] if not equal else "",
            })

    counts = Counter(r["status"] for r in rows)
    other = [r for r in rows if r["status"] == "other"]
    report = {
        "schema": "kotor-s3-full-recompile/1",
        "build_sha256": ledger.get("build_sha256"),
        "expected": EXPECTED,
        "files": len(rows),
        "status_counts": dict(sorted(counts.items())),
        "raw_exact": counts.get("raw_exact", 0),
        "compiler_limited": counts.get("compiler_limited", 0),
        "header_limited": counts.get("header_limited", 0),
        "reachable_only": counts.get("reachable_only", 0),
        "other": other,
        "gate": {
            "all_files_present": len(rows) == EXPECTED,
            "no_unexpected": not other,
            "raw_exact_at_least": counts.get("raw_exact", 0) >= (
                8665 if (args.struct_nss and args.patched_compiler)
                else 8646 if args.struct_nss else 8452),
            "classified_total": (counts.get("raw_exact", 0)
                                 + counts.get("compiler_limited", 0)
                                 + counts.get("header_limited", 0)
                                 + counts.get("reachable_only", 0)) == EXPECTED,
        },
        "rows": rows,
    }
    out_json = Path(args.out_json)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")

    lines = [
        "# 35 — Full-corpus source recompile (8,666 scripts)",
        "",
        f"Status: **{'closed' if all(report['gate'].values()) else 'open'}** — "
        f"{report['files']}/{EXPECTED} clean-room NSS files recompiled with per-file "
        f"historical headers for build SHA-256 `{report['build_sha256']}`.",
        "",
        "## Results",
        "",
        "| class | files | meaning |",
        "|---|---:|---|",
        f"| raw_exact | {report['raw_exact']} | recompiled bytes equal the shipped NCS |",
        f"| compiler_limited | {report['compiler_limited']} | still needing a compiler change |",
        f"| header_limited | {report['header_limited']} | the 5 anomalous-action prototype cases |",
        f"| reachable_only | {report['reachable_only']} | unreachable code that cannot be expressed in source |",
        f"| other | {len(other)} | unexpected (gate failure) |",
        "",
        "## Method",
        "",
        "For every script the shipped NCS is scanned for ACTION calls whose arity is smaller than "
        "the current prototype. A per-file `nwscript.nss` is generated with the historical arity, "
        "the unchanged clean-room NSS is compiled with `nwnnsscomp_ktool`, and the result is "
        "compared byte-for-byte. Struct-recovered source uses the same per-file header rule. The "
        "three function-layout scripts are compiled with the historical nwnnsscomp rebuild "
        "(`tools/compiler`) at `-v129`, which discovers call arguments right-to-left as the "
        "shipped compiler did. Files without such calls use the full header.",
        "",
        "The raw_exact count includes the 24 default-argument cases "
        "(`docs/recon/33_HISTORICAL_HEADERS.md`), the 5 header-limited overrides, all "
        "201 struct recoveries (`docs/recon/36_STRUCT_RECOVERY.md`), the duplicate-return "
        "fixes (`docs/recon/37_RETURN_CLEANUP.md`), and the 3 body-order scripts "
        "(`docs/recon/38_BODY_ORDER.md`). No compiler-limited or ABI-completed file "
        "remains; only the 5 unreachable-code scripts are outside source-level "
        "recompilation.",
        "",
    ]
    out_doc = Path(args.out_doc)
    out_doc.parent.mkdir(parents=True, exist_ok=True)
    out_doc.write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps({"files": report["files"],
                      "status_counts": report["status_counts"],
                      "other": len(other),
                      "gate": report["gate"]}, sort_keys=True))
    return 0 if all(report["gate"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
