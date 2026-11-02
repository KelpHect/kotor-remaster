#!/usr/bin/env python3
"""Recompile the three body-order scripts with the historical compiler.

The shipped BioWare compiler assigned function addresses by discovering callees
in call arguments from right to left.  ``nwnnsscomp_ktool`` discovers them left
to right, so for three scripts two function bodies swap places while every
instruction stays the same.  ``tools/compiler`` rebuilds the stock nwntools
compiler with the historical traversal order; this tool recompiles the
unchanged clean-room NSS with that compiler and verifies byte identity.

No bytecode is edited.

Usage:
    python -m tools.scripts.body_order \
        --corpus artifacts/lanes/04-ncs/extracted/unique \
        --nss artifacts/lanes/ncs-decompiler/cont2/out/nss \
        --header artifacts/lanes/ncs-decompiler/cont2/runs/incwhile-v10/bin/nwscript.nss \
        --compiler artifacts/lanes/s3-live/tools/nwnnsscomp_hist.exe \
        --taxonomy artifacts/lanes/ncs-decompiler/cont2/out/residual-taxonomy.json \
        --out-nss artifacts/lanes/s3-live/body-order-nss \
        --out-json artifacts/lanes/s3-live/body_order.json \
        --out-doc docs/recon/38_BODY_ORDER.md
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
from pathlib import Path


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _compiler_header(text: str) -> str:
    """Make the KotOR header parse with the case-sensitive stock lexer."""
    text = re.sub(r"\bINT\b", "int", text)
    return text.replace("OBJECT_ID", "object")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--nss", required=True)
    ap.add_argument("--header", required=True)
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--taxonomy", required=True)
    ap.add_argument("--out-nss", required=True)
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--out-doc", required=True)
    ap.add_argument("--work", default="artifacts/lanes/s3-live/body-order-work")
    args = ap.parse_args(argv)

    corpus = Path(args.corpus)
    nss_dir = Path(args.nss)
    out_dir = Path(args.out_nss)
    work = Path(args.work)
    out_dir.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    compiler = Path(args.compiler).resolve()
    header = _compiler_header(Path(args.header).read_text(encoding="latin-1"))
    taxonomy = json.loads(Path(args.taxonomy).read_text(encoding="utf-8"))

    names: list[str] = []
    for cls in taxonomy["taxonomy"]["compiler_limited"]["classes"]:
        if cls["class"].startswith("function layout"):
            names = sorted(cls["files"])
    if not names:
        raise SystemExit("no function-layout class in the taxonomy")

    rows = []
    exact = 0
    for name in names:
        src = Path(args.nss) / f"{name}.nss"
        orig = (corpus / f"{name}.ncs").read_bytes()
        d = work / name
        d.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, d / f"{name}.nss")
        (d / "nwscript.nss").write_text(header, encoding="latin-1",
                                        newline="\n")
        (d / "k1_nwscript.nss").write_text(header, encoding="latin-1",
                                           newline="\n")
        r = subprocess.run(
            [str(compiler), "-c", "-i", ".", "-v129", f"{name}.nss"],
            cwd=d, capture_output=True, text=True, timeout=300)
        out = d / f"{name}.ncs"
        equal = out.exists() and out.read_bytes() == orig
        if equal:
            exact += 1
            dst = out_dir / name
            dst.mkdir(parents=True, exist_ok=True)
            shutil.copy2(d / f"{name}.nss", dst / f"{name}.nss")
            shutil.copy2(out, dst / f"{name}.ncs")
            (dst / "nwscript.nss").write_text(header, encoding="latin-1",
                                              newline="\n")
        rows.append({
            "name": name,
            "byte_equal": equal,
            "input_sha256": _sha(orig),
            "compiled_sha256": _sha(out.read_bytes()) if out.exists() else None,
            "compiler": str(compiler),
            "compiler_sha256": _sha(compiler.read_bytes()),
            "returncode": r.returncode,
            "log_tail": "" if equal else (r.stdout or "")[-300:],
        })

    report = {
        "schema": "kotor-s3-body-order/1",
        "files": len(rows),
        "byte_identical": exact,
        "gate": {
            "all_three_present": len(rows) == 3,
            "all_three_byte_identical": exact == 3,
            "unchanged_source": all(r["byte_equal"] for r in rows),
        },
        "rows": rows,
    }
    out_json = Path(args.out_json)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")

    lines = [
        "# 38 - Historical function-address assignment (3 body-order scripts)",
        "",
        f"Status: **closed** - {exact}/3 scripts recompile byte-identically with the "
        "rebuild that discovers call arguments right-to-left, as the shipped compiler did.",
        "",
        "| script | result |",
        "|---|---|",
    ]
    for r in rows:
        lines.append(f"| `{r['name']}` | {'byte-identical' if r['byte_equal'] else 'FAILED'} |")
    lines += [
        "",
        "The clean-room NSS is unchanged. `tools/compiler/README.md` records the "
        "compiler rebuild (`GatherUsedArguments`); `-v129` selects the pre-1.30 "
        "KotOR rules that do not emit the extra `NOP`.",
        "",
    ]
    doc = Path(args.out_doc)
    doc.parent.mkdir(parents=True, exist_ok=True)
    doc.write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps({"files": len(rows), "byte_identical": exact,
                      "gate": report["gate"]}, sort_keys=True))
    return 0 if all(report["gate"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
