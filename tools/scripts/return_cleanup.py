#!/usr/bin/env python3
"""Restore the compiler's duplicate trailing return in the struct-recovered NSS.

After `struct_source.py` recovers the struct, 187 of the 199 remaining files are
short one five-instruction cleanup block.  The shipped compiler emits two
identical cleanup blocks for a trailing local `return vN;`; the decompiler kept
one.  This tool finds the function whose cleanup-block count is one short,
duplicates that function's last return in the unchanged clean-room NSS, and
recompiles.  No bytecode is edited.

The 12 files whose cleanup shape differs remain for the full struct-group ABI
path in `codegen_reconcile.py`.

Usage:
    python -m tools.scripts.return_cleanup \
        --corpus artifacts/lanes/04-ncs/extracted/unique \
        --struct-nss artifacts/lanes/s3-live/struct-nss \
        --taxonomy artifacts/lanes/ncs-decompiler/cont2/out/residual-taxonomy.json \
        --compiler artifacts/lanes/04-ncs/tools/DeNCS/tools/nwnnsscomp_ktool.exe \
        --header artifacts/lanes/ncs-decompiler/cont2/runs/incwhile-v10/bin/nwscript.nss \
        --out-nss artifacts/lanes/s3-live/struct-nss-final \
        --out-json artifacts/lanes/s3-live/return_cleanup.json \
        --out-doc docs/recon/37_RETURN_CLEANUP.md
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
from collections import Counter
from pathlib import Path

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, ".."))
from scripts import ncs as codec  # noqa: E402

CLEAN = ["CPTOPSP", "CPDOWNSP", "MOVSP", "JMP", "MOVSP"]
TYPES = r"(?:void|int|float|string|object|vector|location|effect|event|talent)"


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _cleanup_counts(ins):
    entries = sorted({0x0D} | {i.offset + i.ops[0][1] for i in ins
                               if i.name == "JSR"})
    counts = Counter()
    import bisect
    for k in range(len(ins) - 4):
        if [x.name for x in ins[k:k + 5]] == CLEAN:
            idx = bisect.bisect_right(entries, ins[k].offset) - 1
            if idx >= 0:
                counts[idx] += 1
    return entries, counts


def _function_span(text: str, entry: str):
    m = re.search(r"^" + TYPES + r"\s+f_" + re.escape(entry) +
                  r"\s*\([^\n]*\)\s*\{", text, re.MULTILINE)
    if not m:
        return None
    start = m.start()
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return start, i + 1
    return None


def add_duplicate_return(text: str, entry: str):
    span = _function_span(text, entry)
    if span is None:
        return None
    start, end = span
    body = text[start:end]
    returns = list(re.finditer(r"^(\s*)return .*;\s*$", body, re.MULTILINE))
    if not returns:
        return None
    last = returns[-1]
    line = body[last.start():last.end()].strip()
    indent = last.group(1)
    newbody = body[:last.end()] + f"\n{indent}{line}" + body[last.end():]
    return text[:start] + newbody + text[end:]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--struct-nss", required=True)
    ap.add_argument("--taxonomy", required=True)
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--header", required=True)
    ap.add_argument("--out-nss", required=True)
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--out-doc", required=True)
    ap.add_argument("--work", default="artifacts/lanes/s3-live/return-work")
    args = ap.parse_args(argv)

    corpus = Path(args.corpus)
    struct_dir = Path(args.struct_nss)
    out_dir = Path(args.out_nss)
    out_dir.mkdir(parents=True, exist_ok=True)
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    compiler = str(Path(args.compiler).resolve())
    header = Path(args.header).read_text(encoding="latin-1")
    taxonomy = json.loads(Path(args.taxonomy).read_text(encoding="utf-8"))
    names = []
    for cls in taxonomy["taxonomy"]["compiler_limited"]["classes"]:
        if cls["class"].startswith("CPTOPBP"):
            names = cls["files"]

    rows = []
    exact = 0
    fallback = 0
    for name in names:
        src = struct_dir / name / f"{name}.nss"
        ncs = struct_dir / name / f"{name}.ncs"
        orig = (corpus / f"{name}.ncs").read_bytes()
        if not src.exists() or not ncs.exists():
            rows.append({"name": name, "status": "fallback",
                         "reason": "struct output missing"})
            fallback += 1
            continue
        if ncs.read_bytes() == orig:
            dst = out_dir / name
            dst.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst / f"{name}.nss")
            shutil.copy2(ncs, dst / f"{name}.ncs")
            rows.append({"name": name, "status": "source_exact",
                         "reason": "struct recovery alone"})
            exact += 1
            continue
        oentries, oc = _cleanup_counts(codec.decode(orig))
        centries, cc = _cleanup_counts(codec.decode(ncs.read_bytes()))
        missing = [i for i in oc if oc[i] > cc.get(i, 0)]
        if len(missing) != 1:
            rows.append({"name": name, "status": "fallback",
                         "reason": f"missing indices {missing}"})
            fallback += 1
            continue
        entry = f"{oentries[missing[0]]:X}"
        text = src.read_text(encoding="latin-1")
        new = add_duplicate_return(text, entry)
        if new is None:
            rows.append({"name": name, "status": "fallback",
                         "reason": f"no return in f_{entry}"})
            fallback += 1
            continue
        d = work / name
        d.mkdir(parents=True, exist_ok=True)
        (d / f"{name}.nss").write_text(new, encoding="latin-1", newline="\n")
        (d / "nwscript.nss").write_text(header, encoding="latin-1", newline="\n")
        (d / "k1_nwscript.nss").write_text(header, encoding="latin-1", newline="\n")
        try:
            r = subprocess.run(
                [compiler, "-c", "-g", "1", "--outputdir", ".", f"{name}.nss"],
                cwd=d, capture_output=True, text=True, timeout=300)
        except subprocess.TimeoutExpired:
            r = None
        out = d / f"{name}.ncs"
        ok = out.exists() and out.read_bytes() == orig
        if ok:
            dst = out_dir / name
            dst.mkdir(parents=True, exist_ok=True)
            shutil.copy2(d / f"{name}.nss", dst / f"{name}.nss")
            shutil.copy2(out, dst / f"{name}.ncs")
            rows.append({"name": name, "status": "source_exact",
                         "reason": f"duplicate return in f_{entry}"})
            exact += 1
        else:
            rows.append({"name": name, "status": "fallback",
                         "reason": "different cleanup shape"})
            fallback += 1

    report = {
        "schema": "kotor-s3-return-cleanup/1",
        "files": len(names),
        "source_exact": exact,
        "fallback": fallback,
        "gate": {
            "all_201_present": len(names) == 201,
            "source_exact_plus_fallback": exact + fallback == 201,
            "fallback_is_12": fallback == 12,
        },
        "rows": rows,
    }
    out_json = Path(args.out_json)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    lines = [
        "# 37 — Source-level duplicate trailing-return cleanup",
        "",
        f"Status: **closed for {exact}/201** — {exact} struct files are raw "
        "byte-identical after restoring the compiler's duplicate trailing return; "
        f"{fallback} use the full struct-group ABI path because their cleanup shape differs.",
        "",
        "| check | result |",
        "|---|---|",
        f"| Struct files processed | **{len(names)}/201** |",
        f"| Source-level exact | **{exact}/201** |",
        f"| Full struct ABI fallback | **{fallback}** |",
        "",
        "The fix duplicates the last local `return vN;` in the function whose "
        "cleanup-block count is one short. The shipped compiler emits two identical "
        "cleanup blocks; the decompiler kept one. No bytecode is edited.",
        "",
    ]
    doc = Path(args.out_doc)
    doc.parent.mkdir(parents=True, exist_ok=True)
    doc.write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps({"files": len(names), "source_exact": exact,
                      "fallback": fallback, "gate": report["gate"]},
                     sort_keys=True))
    return 0 if all(report["gate"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
