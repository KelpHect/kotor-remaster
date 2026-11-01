"""Build a project ``gff.Gff`` from in-memory bytes (module RIM / BIF payloads).

``tools/formats/gff.py`` is imported read-only and only accepts a path, so the
payload is staged in a temp file outside the install and deleted immediately
after parse (the parser reads the whole file into memory in __init__).
"""

from __future__ import annotations

import os
import tempfile

from . import load_module

gff = load_module("s1_08_gff", "tools/formats/gff.py")


def gff_from_bytes(data: bytes, name: str = "resource.gff") -> "gff.Gff":
    fd, path = tempfile.mkstemp(prefix="s1_08_", suffix=".gff")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        return gff.Gff(path)
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def list_field(g: "gff.Gff", sindex: int, label: str) -> list[dict]:
    """All structs of the list field `label` on struct `sindex`, as dicts."""
    for f in g.fields_of(sindex):
        if f.type == 15 and f.label == label:
            _, idx = g.list_of(f)
            return [g.as_dict(si) for si in idx]
    return []


def struct_field(g: "gff.Gff", sindex: int, label: str) -> dict | None:
    for f in g.fields_of(sindex):
        if f.type == 14 and f.label == label:
            v = g.value(f)
            return g.as_dict(v["__struct__"])
    return None
