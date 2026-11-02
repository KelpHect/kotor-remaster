#!/usr/bin/env python3
"""Recompile the default-argument residuals with historical prototype headers.

The shipped compiler omitted trailing default arguments at some call sites.
``nwnnsscomp_ktool`` materializes them when the prototype declares them, so the
clean-room source recompiles to a different ``argc``.  The decompiled source
already has the call site arity; the mismatch is the prototype, not the source.

For each affected script this tool:

1. reads the shipped NCS to find ACTION calls whose arity is smaller than the
   shipped prototype;
2. builds a per-file ``nwscript.nss`` that declares that action with only the
   historical parameter count;
3. recompiles the unchanged clean-room NSS with ``nwnnsscomp_ktool``;
4. verifies the result is byte-identical to the shipped NCS.

This is a source-level/compiler-input fix: no bytecode rewriting.

Usage:
    python -m tools.scripts.historical_headers \
        --corpus artifacts/lanes/04-ncs/extracted/unique \
        --nss artifacts/lanes/ncs-decompiler/cont2/out/nss \
        --header artifacts/lanes/ncs-decompiler/cont2/runs/incwhile-v10/bin/nwscript.nss \
        --compiler artifacts/lanes/04-ncs/tools/DeNCS/tools/nwnnsscomp_ktool.exe \
        --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
        --corpus-json artifacts/lanes/s1-04-scripts/corpus.json \
        --taxonomy artifacts/lanes/ncs-decompiler/cont2/out/residual-taxonomy.json \
        --out-json artifacts/lanes/s3-live/historical_headers.json \
        --out-doc docs/recon/33_HISTORICAL_HEADERS.md
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
from pathlib import Path

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, ".."))
from scripts import ncs  # noqa: E402

EXPECTED = 24


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _split_params(text: str) -> list[str]:
    out: list[str] = []
    cur: list[str] = []
    depth = 0
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            item = "".join(cur).strip()
            if item:
                out.append(item)
            cur = []
        else:
            cur.append(ch)
    item = "".join(cur).strip()
    if item:
        out.append(item)
    return out


def _trim_prototype(line: str, name: str, arity: int) -> str | None:
    m = re.search(r"\b" + re.escape(name) + r"\s*\((.*)\)\s*;", line)
    if not m:
        return None
    params = _split_params(m.group(1))
    if arity > len(params):
        return None
    return line[:m.start(1)] + ", ".join(params[:arity]) + line[m.end(1):]


def _trims(orig_bytes: bytes, spec_by_id: dict) -> dict[int, int]:
    out: dict[int, int] = {}
    for ins in ncs.decode(orig_bytes):
        if ins.name != "ACTION":
            continue
        blob = ins.ops[0][1]
        aid = (blob[0] << 8) | blob[1]
        argc = blob[2]
        spec = spec_by_id.get(aid)
        if spec is not None and argc < len(spec["params"]):
            out[aid] = argc
    return out


def _header_for(full_lines: list[str], trims: dict[int, int],
                spec_by_id: dict) -> tuple[str, list[dict]]:
    lines = list(full_lines)
    applied = []
    for aid, arity in trims.items():
        name = spec_by_id[aid]["name"]
        changed = False
        for k, line in enumerate(lines):
            if (re.search(r"\b" + re.escape(name) + r"\s*\(", line)
                    and line.strip().endswith(";")):
                new = _trim_prototype(line, name, arity)
                if new is not None and new != line:
                    lines[k] = new
                    changed = True
                    break
        applied.append({"action": aid, "name": name, "arity": arity,
                        "applied": changed})
    return "\n".join(lines) + "\n", applied


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--nss", required=True)
    ap.add_argument("--header", required=True)
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--specs", required=True)
    ap.add_argument("--corpus-json", required=True)
    ap.add_argument("--taxonomy", required=True)
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--out-doc", required=True)
    ap.add_argument("--work", default="artifacts/lanes/s3-live/historical-work")
    args = ap.parse_args(argv)

    corpus = Path(args.corpus)
    nss_dir = Path(args.nss)
    compiler = Path(args.compiler)
    work = Path(args.work)
    specs = json.loads(Path(args.specs).read_text(encoding="utf-8"))
    spec_by_id = {a["id"]: a for a in specs["actions"]}
    ledger = json.loads(Path(args.corpus_json).read_text(encoding="utf-8"))
    taxonomy = json.loads(Path(args.taxonomy).read_text(encoding="utf-8"))
    target = set()
    for cls in taxonomy["taxonomy"]["compiler_limited"]["classes"]:
        if cls["class"].startswith("action default-argument"):
            target.update(cls["files"])
    full_lines = Path(args.header).read_text(encoding="latin-1").splitlines()

    candidates = []
    for row in ledger["scripts"]:
        if (row.get("decompile") or {}).get("class") != "compiler-limited":
            continue
        name = row["decompile"]["name"]
        if name not in target:
            continue
        op = corpus / f"{name}.ncs"
        if not op.exists():
            continue
        trims = _trims(op.read_bytes(), spec_by_id)
        if trims:
            candidates.append((name, trims))
    candidates.sort()

    rows = []
    for name, trims in candidates:
        header_text, applied = _header_for(full_lines, trims, spec_by_id)
        d = work / name
        d.mkdir(parents=True, exist_ok=True)
        (d / "nwscript.nss").write_text(header_text, encoding="latin-1",
                                        newline="\n")
        (d / "k1_nwscript.nss").write_text(header_text, encoding="latin-1",
                                           newline="\n")
        shutil.copy2(compiler, d / compiler.name)
        shutil.copy2(nss_dir / f"{name}.nss", d / f"{name}.nss")
        r = subprocess.run(
            [str(d / compiler.name), "-c", "-g", "1", "--outputdir", ".",
             f"{name}.nss"],
            cwd=d, capture_output=True, text=True, timeout=300)
        out = d / f"{name}.ncs"
        orig = (corpus / f"{name}.ncs").read_bytes()
        equal = out.exists() and out.read_bytes() == orig
        rows.append({
            "name": name,
            "trims": [{"action": t["action"], "name": t["name"],
                       "arity": t["arity"], "applied": t["applied"]}
                      for t in applied],
            "input_sha256": _sha(orig),
            "recompiled_sha256": _sha(out.read_bytes()) if out.exists() else None,
            "byte_equal": equal,
            "compiler_returncode": r.returncode,
            "log_tail": (r.stdout or "")[-300:],
        })

    failures = [r for r in rows if not r["byte_equal"]]
    report = {
        "schema": "kotor-s3-historical-headers/1",
        "build_sha256": ledger.get("build_sha256"),
        "expected": EXPECTED,
        "files": len(rows),
        "byte_identical": len(rows) - len(failures),
        "failures": failures,
        "gate": {
            "all_expected_present": len(rows) == EXPECTED,
            "all_byte_identical": not failures,
        },
        "rows": rows,
    }
    out_json = Path(args.out_json)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")

    lines = [
        "# 33 — Historical prototype headers for the default-argument residuals",
        "",
        f"Status: **{'closed' if all(report['gate'].values()) else 'open'}** — "
        f"{report['byte_identical']}/{EXPECTED} recompiled byte-identically from the "
        f"unchanged clean-room source for build SHA-256 `{report['build_sha256']}`.",
        "",
        "## Gate",
        "",
        "| check | result |",
        "|---|---|",
        f"| Affected scripts found | **{report['files']}/{EXPECTED}** |",
        f"| Raw byte-identical recompiles | **{report['byte_identical']}/{EXPECTED}** |",
        f"| Failures | **{len(failures)}** |",
        "",
        "## Method",
        "",
        "The decompiled source call sites already have the historical arity. The shipped "
        "compiler omitted trailing defaults; `nwnnsscomp_ktool` materializes them from the "
        "prototype. For each script this tool builds a per-file `nwscript.nss` whose affected "
        "action prototype has exactly the historical parameter count, recompiles the unchanged "
        "NSS, and compares the bytes. No bytecode is rewritten.",
        "",
    ]
    for row in rows:
        trims = ", ".join(f"{t['name']} argc={t['arity']}" for t in row["trims"])
        lines.append(f"- `{row['name']}` — {trims} — **{'equal' if row['byte_equal'] else 'FAIL'}**")
    lines += [
        "",
        "## Reproduce",
        "",
        "```bash",
        "python -m tools.scripts.historical_headers \\",
        "  --corpus artifacts/lanes/04-ncs/extracted/unique \\",
        "  --nss artifacts/lanes/ncs-decompiler/cont2/out/nss \\",
        "  --header artifacts/lanes/ncs-decompiler/cont2/runs/incwhile-v10/bin/nwscript.nss \\",
        "  --compiler artifacts/lanes/04-ncs/tools/DeNCS/tools/nwnnsscomp_ktool.exe \\",
        "  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \\",
        "  --corpus-json artifacts/lanes/s1-04-scripts/corpus.json \\",
        "  --taxonomy artifacts/lanes/ncs-decompiler/cont2/out/residual-taxonomy.json \\",
        "  --out-json artifacts/lanes/s3-live/historical_headers.json \\",
        "  --out-doc docs/recon/33_HISTORICAL_HEADERS.md",
        "```",
        "",
    ]
    out_doc = Path(args.out_doc)
    out_doc.parent.mkdir(parents=True, exist_ok=True)
    out_doc.write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps({"files": report["files"],
                      "byte_identical": report["byte_identical"],
                      "failures": len(failures),
                      "gate": report["gate"]}, sort_keys=True))
    return 0 if all(report["gate"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
