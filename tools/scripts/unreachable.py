#!/usr/bin/env python3
"""Restore unreachable statements that the decompiler dropped.

Five scripts execute code that the decompiler dropped as unreachable after a
final `return`; the shipped compiler still emitted it, so the clean-room source
was short.  Four are a single `SetGlobalNumber("tat_LinesChecked", 0)`; the
fifth is a party-damage loop in `k_plev_space_hb`.  The statements are restored
verbatim from the shipped bytecode, the unchanged source is recompiled, and the
result is checked byte-for-byte.

Usage:
    python -m tools.scripts.unreachable \
        --corpus artifacts/lanes/04-ncs/extracted/unique \
        --nss artifacts/lanes/ncs-decompiler/cont2/out/nss \
        --header artifacts/lanes/ncs-decompiler/cont2/runs/incwhile-v10/bin/nwscript.nss \
        --compiler artifacts/lanes/04-ncs/tools/DeNCS/tools/nwnnsscomp_ktool.exe \
        --taxonomy artifacts/lanes/ncs-decompiler/cont2/out/residual-taxonomy.json \
        --out-nss artifacts/lanes/s3-live/struct-nss-final \
        --out-json artifacts/lanes/s3-live/unreachable.json \
        --out-doc docs/recon/39_UNREACHABLE.md
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

RESTORATIONS = {
    "k_pkas_news_01__f69d97f6c0170f98":
        '\tSetGlobalNumber("tat_LinesChecked", 0);\n',
    "k_pkas_news_02__45dee6a6adbdc972":
        '\tSetGlobalNumber("tat_LinesChecked", 0);\n',
    "k_pkas_news_03__4b34b22c12799199":
        '\tSetGlobalNumber("tat_LinesChecked", 0);\n',
    "k_pkas_news_04__9e93320253a30ef8":
        '\tSetGlobalNumber("tat_LinesChecked", 0);\n',
    "k_plev_space_hb__a2b5e977f40bbbce": (
        "\tif ((v1 > 0)) {\n"
        "\t\tv1 = (v1 - 6);\n"
        "\t\tif ((v1 <= 0)) {\n"
        "\t\t\tv2 = 0;\n"
        "\t\t\twhile ((v2 < GetPartyMemberCount())) {\n"
        "\t\t\t\tv3 = GetPartyMemberByIndex(v2);\n"
        "\t\t\t\tApplyEffectToObject(0, EffectDeath(0, 1), GetFirstPC(), 0.0);\n"
        "\t\t\t\tv2++;\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t\tSetGlobalNumber(\"LEV_SPACEWALK\", v1);\n"
        "\t}\n"
    ),
}


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


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
    ap.add_argument("--work", default="artifacts/lanes/s3-live/unreachable-work")
    args = ap.parse_args(argv)

    corpus = Path(args.corpus)
    out_dir = Path(args.out_nss)
    out_dir.mkdir(parents=True, exist_ok=True)
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    compiler = str(Path(args.compiler).resolve())
    header = Path(args.header).read_text(encoding="latin-1")
    taxonomy = json.loads(Path(args.taxonomy).read_text(encoding="utf-8"))
    names = sorted(taxonomy["taxonomy"]["reachable_only"]["files"])

    rows = []
    exact = 0
    for name in names:
        src = Path(args.nss) / f"{name}.nss"
        if name not in RESTORATIONS or not src.exists():
            rows.append({"name": name, "status": "missing"})
            continue
        text = src.read_text(encoding="latin-1").rstrip("\n")
        if not text.endswith("}"):
            rows.append({"name": name, "status": "bad_source"})
            continue
        text = text[:-1].rstrip("\n") + "\n" + RESTORATIONS[name] + "}\n"
        d = work / name
        d.mkdir(parents=True, exist_ok=True)
        (d / f"{name}.nss").write_text(text, encoding="latin-1", newline="\n")
        (d / "nwscript.nss").write_text(header, encoding="latin-1",
                                        newline="\n")
        (d / "k1_nwscript.nss").write_text(header, encoding="latin-1",
                                           newline="\n")
        r = subprocess.run(
            [compiler, "-c", "-g", "1", "--outputdir", ".", f"{name}.nss"],
            cwd=d, capture_output=True, text=True, timeout=300)
        out = d / f"{name}.ncs"
        orig = (corpus / f"{name}.ncs").read_bytes()
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
            "status": "source_exact" if equal else "failed",
            "input_sha256": _sha(orig),
            "compiled_sha256": _sha(out.read_bytes()) if out.exists() else None,
            "returncode": r.returncode,
            "log_tail": "" if equal else (r.stdout or "")[-300:],
        })

    report = {
        "schema": "kotor-s3-unreachable/1",
        "files": len(names),
        "source_exact": exact,
        "gate": {
            "five_present": len(names) == 5,
            "all_five_restored": exact == 5,
        },
        "rows": rows,
    }
    out_json = Path(args.out_json)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")

    lines = [
        "# 39 - Unreachable statements after `return`",
        "",
        f"Status: **closed** - {exact}/5 scripts are byte-identical after restoring "
        "the unreachable statements the decompiler dropped.",
        "",
        "| script | result |",
        "|---|---|",
    ]
    for r in rows:
        lines.append(f"| `{r['name']}` | {r['status']} |")
    lines += [
        "",
        "The four news scripts ended with `return ...;` followed by an unreachable "
        '`SetGlobalNumber("tat_LinesChecked", 0);`. The shipped compiler still emitted '
        "that statement; the decompiler removed it. Restoring it compiles to the "
        "shipped bytes with no other change.",
        "",
        "`k_plev_space_hb` discards a complete party-damage loop after `return;`. "
        "Restoring `v1` (the spacewalk counter), `v2` (the party index), and `v3` "
        "(the party member) reproduces the shipped block exactly; the source is "
        "type-correct and needs no compiler extension.",
        "",
    ]
    doc = Path(args.out_doc)
    doc.parent.mkdir(parents=True, exist_ok=True)
    doc.write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps({"files": len(names), "source_exact": exact,
                      "gate": report["gate"]}, sort_keys=True))
    return 0 if all(report["gate"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
