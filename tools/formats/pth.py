#!/usr/bin/env python3
"""PTH (path graph) parser/validator plus the shared GFF byte helpers.

KotOR PTH resources (type 3003) are GFF V3.2 files (magic ``PTH ``,
version ``V3.2``) whose root struct carries two lists:

* ``Path_Points``    -- structs with ``X``, ``Y`` (floats, world space),
  ``First_Conection`` (u32) and ``Conections`` (u32, the shipped key keeps
  the typo).  ``First_Conection + Conections`` indexes into
  ``Path_Conections``.
* ``Path_Conections`` -- structs with ``Destination`` (u32 point index).

This module also hosts two GFF-level helpers used across the lane:

* :func:`load_gff_bytes` -- parse a GFF payload from memory with the
  project reader ``tools/formats/gff.py`` (read-only import).
* :func:`gff_reencode` -- layout-preserving re-serializer.  It rebuilds
  the 56-byte header and every table row from the *parsed* model while
  keeping the raw field-data / field-index / list-index arenas (shipped
  files carry stale unreferenced arena bytes that a semantic writer would
  drop).  For every shipped file the sections are contiguous (enforced by
  ``gff.check``), so the output is byte-identical to the input; the
  round-trip suite asserts exactly that.

Verified against the shipped corpus (see docs/recon/18_FORMAT_MATRIX.md):
132 PTH resources parse and validate; the GFF re-encoder is byte-identical
on all 11,143 shipped GFF payloads.  Build of record:
SHA-256 0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import os
import struct
import sys
import tempfile
from dataclasses import dataclass, field

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import gff  # noqa: E402  (project reader, read-only import)


class PthError(ValueError):
    pass


def load_gff_bytes(data: bytes, name: str = "<memory>") -> "gff.Gff":
    """Parse GFF bytes with the project reader (via a private temp file)."""
    fd, tmp = tempfile.mkstemp(prefix="s102_gff_", suffix=".bin")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        g = gff.Gff(tmp)
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass
    g.path = name
    return g


def gff_reencode(g: "gff.Gff") -> bytes:
    """Re-serialize a parsed GFF layout-preservingly.

    Rebuilds the header and the struct/field/label tables from the parsed
    rows and copies the field-data, field-index and list-index arenas
    verbatim (including stale entries present in shipped files).  Raises
    ValueError if the parsed sections are not contiguous in the canonical
    order (in which case byte-identity is impossible and the caller must
    fall back to a semantic comparison).
    """
    b = g.buf
    # Canonical section order with the 56-byte header; verified contiguous
    # by gff.check() for the shipped corpus.
    expected = [
        (g.struct_off, g.struct_cnt * 12),
        (g.field_off, g.field_cnt * 12),
        (g.label_off, g.label_cnt * 16),
        (g.fdata_off, g.fdata_len),
        (g.fidx_off, g.fidx_len),
        (g.lidx_off, g.lidx_len),
    ]
    pos = 56
    for off, length in expected:
        if off != pos:
            raise ValueError(
                f"non-contiguous GFF sections: expected section at {pos}, found {off}")
        pos = off + length
    if pos != len(b):
        raise ValueError(f"GFF sections end at {pos}, file is {len(b)} bytes")

    struct_rows = []
    for s in g.structs:
        if s.count == 0:
            struct_rows.append((s.type_id, 0, 0))
        elif s.single:
            struct_rows.append((s.type_id, s.rows[0], 1))
        else:
            struct_rows.append((s.type_id, s.slot * 4, s.count))
    field_rows = [(f.type, f.label_index, f.data) for f in g.fields]

    struct_bytes = b"".join(struct.pack("<3I", *row) for row in struct_rows)
    field_bytes = b"".join(struct.pack("<3I", *row) for row in field_rows)
    label_bytes = b[g.label_off:g.label_off + g.label_cnt * 16]
    fdata = g.fdata
    fidx = b[g.fidx_off:g.fidx_off + g.fidx_len]
    lidx = g.lidx

    struct_off = 56
    field_off = struct_off + len(struct_bytes)
    label_off = field_off + len(field_bytes)
    fdata_off = label_off + len(label_bytes)
    fidx_off = fdata_off + len(fdata)
    lidx_off = fidx_off + len(fidx)
    header = struct.pack(
        "<12I",
        struct_off, len(struct_rows),
        field_off, len(field_rows),
        label_off, g.label_cnt,
        fdata_off, len(fdata),
        fidx_off, len(fidx),
        lidx_off, len(lidx),
    )
    return g.magic + g.version + header + struct_bytes + field_bytes + label_bytes \
        + bytes(fdata) + fidx + bytes(lidx)


@dataclass(frozen=True)
class PathPoint:
    x: float
    y: float
    first_connection: int
    connection_count: int


@dataclass(frozen=True)
class PathConnection:
    destination: int


@dataclass
class Pth:
    points: tuple[PathPoint, ...]
    connections: tuple[PathConnection, ...]
    gff: "gff.Gff" = field(repr=False)


def parse_pth(data: bytes, name: str = "<memory>") -> Pth:
    """Parse and strictly validate a PTH resource payload."""
    g = load_gff_bytes(data, name)
    if g.magic != b"PTH " or g.version != b"V3.2":
        raise PthError(f"{name}: expected 'PTH V3.2', got {g.magic!r} {g.version!r}")
    errs = gff.check(g)
    if errs:
        raise PthError(f"{name}: GFF structural errors: {errs[:4]}")
    verr = gff.check_values(g)
    if verr:
        raise PthError(f"{name}: GFF value errors: {verr[:4]}")

    points: list[PathPoint] = []
    connections: list[PathConnection] = []
    root = g.fields_of(0)
    lists = {f.label: f for f in root if f.type == 15}
    if "Path_Points" not in lists:
        raise PthError(f"{name}: root has no Path_Points list")
    if "Path_Conections" not in lists:
        raise PthError(f"{name}: root has no Path_Conections list")

    _n, cidx = g.list_of(lists["Path_Conections"])
    for si in cidx:
        d = g.as_dict(si)
        if "Destination" not in d:
            raise PthError(f"{name}: connection struct {si} lacks Destination")
        dest = d["Destination"]
        if not isinstance(dest, int) or not 0 <= dest < 0xFFFFFFFF:
            raise PthError(f"{name}: connection struct {si} bad Destination {dest!r}")
        connections.append(PathConnection(dest))

    _n, pidx = g.list_of(lists["Path_Points"])
    for si in pidx:
        d = g.as_dict(si)
        for key in ("X", "Y", "First_Conection", "Conections"):
            if key not in d:
                raise PthError(f"{name}: point struct {si} lacks {key}")
        first, cnt = d["First_Conection"], d["Conections"]
        if first + cnt > len(connections):
            raise PthError(
                f"{name}: point struct {si} connections [{first},+{cnt}) exceed "
                f"{len(connections)} Path_Conections entries")
        points.append(PathPoint(float(d["X"]), float(d["Y"]), first, cnt))

    for c in connections:
        if c.destination >= len(points):
            raise PthError(
                f"{name}: connection destination {c.destination} out of range "
                f"({len(points)} points)")
    return Pth(tuple(points), tuple(connections), g)


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one PTH resource (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    p = parse_pth(data, args.path)
    print(f"{args.path}: points={len(p.points)} connections={len(p.connections)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
