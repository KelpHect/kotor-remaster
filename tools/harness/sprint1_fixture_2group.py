#!/usr/bin/env python3
"""Lane s1-06-dynamic: build a synthetic 2-group MDL from the shipped plc_bed1
(read-only from the install), for the P0-handoff multi-group runtime proof
(docs/recon/23 #11-#13; layout per docs/recon/15).

The shipped mesh node header (type 0x21) carries, per mesh:
  +0x100 face-count array ptr, +0x104/+0x108 counts,
  +0x10c handle array ptr,    +0x110/+0x114 counts,
  +0x118 group-offsets ptr,   +0x11c/+0x120 group count
(all file offsets relative to byte 12).  Shipped models are 1|1|1.  This script
appends a second copy of group 0's arrays/face indices at EOF and points the
node at 2-entry arrays, setting every count field to 2.  The MDX is copied
unchanged (both groups share the vertex block; they differ only in face lists).

Output goes ONLY to the lane artifacts/profile (never the install, never tracked
files).  Usage:
  python tools/harness/sprint1_fixture_2group.py --game-dir <install> \
      --out artifacts/lanes/s1-06-dynamic/fixtures/plc_bed1_2grp \
      --install-override artifacts/lanes/s1-06-dynamic/profile/Override
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "resources"))
sys.path.insert(0, str(HERE.parent / "formats"))
import kotor_archive as ka  # noqa: E402
from mdl import parse_mdl  # noqa: E402

BUILD_SHA256 = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6"


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def find_entry(key, resref: str, rtype: int):
    for e in key.entries:
        if e.resref == resref and e.res_type == rtype:
            return e
    raise SystemExit(f"{resref}/{rtype} not in chitin.key")


def read_key_resource(gd: Path, key, e) -> bytes:
    bif_name = key.bifs[e.bif_index]["name"]
    bif = ka.read_bif(gd / bif_name)
    return bif.read_record(bif.records[e.table_index])


def find_mesh_node(mdl: bytes) -> int:
    import struct as st
    root_off, node_cnt = st.unpack_from("<2I", mdl, 0x34)
    # walk nodes like mdl.parse_mdl does
    sys.path.insert(0, str(HERE.parent / "formats"))
    from mdl import parse_mdl
    m = parse_mdl(mdl, "scan")
    for n in m.nodes:
        if n.type == 0x21:
            return n.offset
    raise SystemExit("no mesh node found")


def build_2group(mdl: bytes, mesh_node_off: int | None = None) -> tuple[bytes, dict]:
    if mesh_node_off is None:
        mesh_node_off = find_mesh_node(mdl)
    """Return (new_mdl_bytes, report) with the mesh node at `mesh_node_off` set to 2 groups."""
    a = 12 + mesh_node_off
    counts_arr, counts_n = struct.unpack_from("<2I", mdl, a + 0x100)
    handles_arr, handles_n = struct.unpack_from("<2I", mdl, a + 0x10C)
    offsets_arr, offsets_n = struct.unpack_from("<2I", mdl, a + 0x118)
    assert counts_n == 1 and handles_n == 1 and offsets_n == 1, "expected shipped 1-group"
    c0 = struct.unpack_from("<I", mdl, 12 + counts_arr)[0]
    h0 = struct.unpack_from("<I", mdl, 12 + handles_arr)[0]
    o0 = struct.unpack_from("<I", mdl, 12 + offsets_arr)[0]
    faces = mdl[12 + o0: 12 + o0 + c0 * 2]
    out = bytearray(mdl)
    base = len(mdl)  # append at EOF; offsets are relative to byte 12
    rel = lambda fileoff: fileoff - 12
    counts2_off = base
    out += struct.pack("<2I", c0, c0)
    handles2_off = len(out)
    out += struct.pack("<2I", h0, h0)
    offsets2_off = len(out)
    faces2_off = len(out) + 8
    out += struct.pack("<2I", o0, rel(faces2_off))
    out += faces
    # patch node header fields
    struct.pack_into("<I", out, a + 0x100, rel(counts2_off))
    struct.pack_into("<I", out, a + 0x104, 2)
    struct.pack_into("<I", out, a + 0x108, 2)
    struct.pack_into("<I", out, a + 0x10C, rel(handles2_off))
    struct.pack_into("<I", out, a + 0x110, 2)
    struct.pack_into("<I", out, a + 0x114, 2)
    struct.pack_into("<I", out, a + 0x118, rel(offsets2_off))
    struct.pack_into("<I", out, a + 0x11C, 2)
    struct.pack_into("<I", out, a + 0x120, 2)
    report = {
        "mesh_node_off": hex(mesh_node_off),
        "group0": {"counts": c0, "handle": hex(h0), "offsets": hex(o0)},
        "appended": {"counts2_off": hex(counts2_off), "handles2_off": hex(handles2_off),
                     "offsets2_off": hex(offsets2_off), "faces2_off": hex(faces2_off)},
        "new_fields": {hex(a + o): v for o, v in ((0x100, rel(counts2_off)), (0x104, 2), (0x108, 2),
                                                  (0x10C, rel(handles2_off)), (0x110, 2), (0x114, 2),
                                                  (0x118, rel(offsets2_off)), (0x11C, 2), (0x120, 2))},
        "old_size": len(mdl), "new_size": len(out),
        "old_sha256": sha(mdl), "new_sha256": sha(bytes(out)),
    }
    return bytes(out), report


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-dir", required=True)
    ap.add_argument("--resref", default="plc_bed1")
    ap.add_argument("--out", required=True)
    ap.add_argument("--install-override", default=None)
    args = ap.parse_args()
    gd = Path(args.game_dir)
    if sha((gd / "swkotor.exe").read_bytes()) != BUILD_SHA256:
        raise SystemExit("install exe hash drift; stop")
    key = ka.read_key(gd / "chitin.key")
    mdl = read_key_resource(gd, key, find_entry(key, args.resref, 2002))
    mdx = read_key_resource(gd, key, find_entry(key, args.resref, 3008))
    new_mdl, report = build_2group(mdl)
    # sanity: our parser still reads it
    m = parse_mdl(new_mdl, args.resref + "-2grp")
    report["mdl_parse"] = {"name": m.name, "nodes": len(m.nodes), "warnings": m.warnings,
                           "mdx_size": m.mdx_size}
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / f"{args.resref}.mdl").write_bytes(new_mdl)
    (out / f"{args.resref}.mdx").write_bytes(mdx)
    (out / "build-report.json").write_text(__import__("json").dumps(report, indent=2) + "\n")
    if args.install_override:
        ov = Path(args.install_override)
        ov.mkdir(parents=True, exist_ok=True)
        shutil.copy2(out / f"{args.resref}.mdl", ov / f"{args.resref}.mdl")
        shutil.copy2(out / f"{args.resref}.mdx", ov / f"{args.resref}.mdx")
        report["override"] = str(ov)
    print(__import__("json").dumps({k: report[k] for k in ("old_size", "new_size", "old_sha256", "new_sha256", "mdl_parse")}, indent=2))
    print("wrote", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
