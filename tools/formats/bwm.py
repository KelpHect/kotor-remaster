#!/usr/bin/env python3
"""Strict parser/validator for KotOR BWM (binary walkmesh) containers.

BWM V1.0 layout (verified against all 1,554 shipped WOK/DWK/PWK resources;
see docs/recon/04, docs/recon/12 and docs/recon/18_FORMAT_MATRIX.md):

    0x00  char[8]  "BWM V1.0"
    0x08  u32      type (1 = area walkmesh WOK, 0 = DWK/PWK)
    0x0C  5 x 3 x f32   float triples: bbox_min, bbox_max, ?, ?, position
                         (the last triple, file offset 0x3C, is the walkmesh
                          node translation == MDL node-545 local position,
                          verified bit-exact 453/453 in docs/recon/12)
    0x48  16 x u32 section fields, as (count, offset) pairs unless noted:
          vert_count, vert_off          vertices: 3 x f32
          face_count, face_off          faces: 3 x u32 indices
          mat_off                       face material/surface ids: face_count x u32
          normal_off                    face normals (type 1): face_count x 3 x f32
          unk_off                       u32 array running until adj_off
          adj_count, adj_off            adjacency records: 44 B each
          aabb_count, aabb_off          AABB-tree nodes: 12 B each
          edge_count, edge_off          perimeter edges: 8 B each
          peri_count, peri_off          4 B each

All offsets are absolute file offsets; the header itself is 136 (0x88)
bytes.  Validation enforces section bounds, the vert->face->mat->normal
chaining observed in shipped files, and face indices < vert_count.
Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass


class BwmError(ValueError):
    pass

MAGIC = b"BWM V1.0"
HEADER_SIZE = 0x88

SECTION_FIELDS = (
    "vert_count", "vert_off",
    "face_count", "face_off",
    "mat_off",
    "normal_off",
    "unk_off",
    "adj_count", "adj_off",
    "unk2",
    "aabb_count", "aabb_off",
    "edge_count", "edge_off",
    "peri_count", "peri_off",
)


@dataclass
class Bwm:
    type: int
    triples: tuple[tuple[float, float, float], ...]  # 5 triples from 0x0C
    position: tuple[float, float, float]             # triple index 4
    fields: dict[str, int]
    verts: tuple[tuple[float, float, float], ...]
    faces: tuple[tuple[int, int, int], ...]
    face_mats: tuple[int, ...]
    normals: tuple[tuple[float, float, float], ...]
    size: int

    @property
    def vert_count(self) -> int:
        return self.fields["vert_count"]

    @property
    def face_count(self) -> int:
        return self.fields["face_count"]


def _section(data: bytes, off: int, count: int, stride: int, what: str, name: str) -> None:
    if count == 0:
        return
    if off < HEADER_SIZE or off + count * stride > len(data):
        raise BwmError(
            f"{name}: {what} section [{off}, {off + count * stride}) "
            f"outside file ({len(data)} bytes, count {count}, stride {stride})")


def parse_bwm(data: bytes, name: str = "<memory>", expect_type: int | None = None) -> Bwm:
    """Parse and strictly validate a BWM payload."""
    if len(data) < HEADER_SIZE:
        raise BwmError(f"{name}: shorter than {HEADER_SIZE}-byte BWM header")
    if data[:8] != MAGIC:
        raise BwmError(f"{name}: bad magic {data[:8]!r}")
    btype = struct.unpack_from("<I", data, 8)[0]
    if expect_type is not None and btype != expect_type:
        raise BwmError(f"{name}: expected BWM type {expect_type}, got {btype}")
    if btype not in (0, 1):
        raise BwmError(f"{name}: unknown BWM type {btype}")
    triples = tuple(
        struct.unpack_from("<3f", data, 0x0C + i * 12) for i in range(5))
    vals = struct.unpack_from("<16I", data, 0x48)
    fields = dict(zip(SECTION_FIELDS, vals))

    vc, vo = fields["vert_count"], fields["vert_off"]
    fc, fo = fields["face_count"], fields["face_off"]
    mo, no, do = fields["mat_off"], fields["normal_off"], fields["unk_off"]
    ac, ao = fields["adj_count"], fields["adj_off"]
    adjc, adjoff = fields["aabb_count"], fields["aabb_off"]
    ec, eo = fields["edge_count"], fields["edge_off"]
    pc, po = fields["peri_count"], fields["peri_off"]
    unk2 = fields["unk2"]

    _section(data, vo, vc, 12, "vertex", name)
    _section(data, fo, fc, 12, "face", name)
    if fc and mo:
        _section(data, mo, fc, 4, "face-material", name)
    if btype == 1 and fc:
        _section(data, no, fc, 12, "face-normal", name)
    _section(data, ao, ac, 44, "adjacency", name)
    _section(data, adjoff, adjc, 12, "aabb", name)
    _section(data, eo, ec, 8, "edge", name)
    _section(data, po, pc, 4, "perimeter", name)
    if do and ao and do > ao:
        raise BwmError(f"{name}: unk_off {do} beyond adj_off {ao}")
    if ao and do and (ao - do) % 4:
        raise BwmError(f"{name}: unk section [{do},{ao}) not u32 aligned")
    # Chaining invariants observed on every shipped file.
    if vc and vo + vc * 12 != fo:
        raise BwmError(f"{name}: vertices do not end at face offset ({vo}+{vc}*12 != {fo})")
    if fc and fo + fc * 12 != mo:
        raise BwmError(f"{name}: faces do not end at mat offset ({fo}+{fc}*12 != {mo})")
    if btype == 1 and fc and mo + fc * 4 != no:
        raise BwmError(f"{name}: face mats do not end at normal offset")
    if btype == 1 and fc and no + fc * 12 != do:
        raise BwmError(f"{name}: normals do not end at unk offset")
    if ac and ao + ac * 44 > adjoff:
        raise BwmError(f"{name}: adjacency overruns aabb offset")
    if adjc and adjoff + adjc * 12 != eo:
        raise BwmError(f"{name}: aabb nodes do not end at edge offset")
    if ec and eo + ec * 8 != po:
        raise BwmError(f"{name}: edges do not end at perimeter offset")
    if pc and po + pc * 4 != len(data):
        raise BwmError(f"{name}: perimeter does not end at EOF "
                       f"({po}+{pc}*4 != {len(data)})")

    verts = tuple(struct.unpack_from("<3f", data, vo + i * 12) for i in range(vc))
    faces = tuple(struct.unpack_from("<3I", data, fo + i * 12) for i in range(fc))
    for fi, f in enumerate(faces):
        for idx in f:
            if idx >= vc:
                raise BwmError(f"{name}: face {fi} references vertex {idx} >= {vc}")
    face_mats = tuple(struct.unpack_from("<%dI" % fc, data, mo)) if fc and mo else ()
    normals = tuple(struct.unpack_from("<3f", data, no + i * 12) for i in range(fc)) \
        if btype == 1 and fc and no else ()
    return Bwm(btype, triples, triples[4], fields, verts, faces, face_mats, normals,
               len(data))


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one BWM/WOK/DWK/PWK resource (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    b = parse_bwm(data, args.path)
    print(f"{args.path}: type={b.type} verts={b.vert_count} faces={b.face_count} "
          f"pos={tuple(round(v, 5) for v in b.position)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
