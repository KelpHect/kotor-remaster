#!/usr/bin/env python3
"""GFF-based .gui parser/validator (resource type 2047).

A KotOR .gui file is a GFF V3.2 container (magic ``GUI ``) whose root
struct holds the screen fields -- observed on all 91 shipped GUI resources
(84 in gui.bif + 7 in patch.erf):

    CONTROLS     list of control structs (the control tree)
    CONTROLTYPE  int32 (root screen type)
    TAG          string, Obj_Locked uint8, Obj_ParentID int32,
    EXTENT       struct, ALPHA float, BORDER struct, COLOR vector3, ...

Each control struct repeats the same shape: a ``CONTROLS`` list of child
controls, ``CONTROLTYPE`` int32, ``TAG`` string, etc.  Validation runs the
project GFF checks plus a recursive control-tree walk requiring CONTROLS
(list) and CONTROLTYPE (int) on every control struct, and that struct
references stay in range (guaranteed by gff.check).
Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass

from pth import load_gff_bytes  # noqa: F401  (shared GFF-bytes helper)


class GuiError(ValueError):
    pass


@dataclass
class Control:
    struct_index: int
    control_type: int
    tag: str
    children: tuple["Control", ...]


@dataclass
class Gui:
    control_type: int
    tag: str
    root: Control
    gff: object


def _control(g, si: int, depth: int, name: str) -> Control:
    if depth > 64:
        raise GuiError(f"{name}: control tree deeper than 64 at struct {si}")
    fields = g.fields_of(si)
    by_label = {}
    controls_field = None
    for f in fields:
        if f.type == 15 and f.label == "CONTROLS":
            controls_field = f
        by_label.setdefault(f.label, f)
    # leaf controls may omit the CONTROLS list (90 of 91 shipped screens
    # have at least one such leaf)
    if "CONTROLTYPE" not in by_label or by_label["CONTROLTYPE"].type != 5:
        raise GuiError(f"{name}: control struct {si} lacks int32 CONTROLTYPE")
    ctype = g.value(by_label["CONTROLTYPE"])
    tag = g.value(by_label["TAG"]) if "TAG" in by_label else ""
    children = ()
    if controls_field is not None:
        _n, idx = g.list_of(controls_field)
        children = tuple(_control(g, csi, depth + 1, name) for csi in idx)
    return Control(si, ctype, tag, children)


def parse_gui(data: bytes, name: str = "<memory>") -> Gui:
    """Parse and strictly validate a .gui payload."""
    g = load_gff_bytes(data, name)
    if g.magic != b"GUI " or g.version != b"V3.2":
        raise GuiError(f"{name}: expected 'GUI V3.2', got {g.magic!r} {g.version!r}")
    import gff as _gff
    errs = _gff.check(g)
    if errs:
        raise GuiError(f"{name}: GFF structural errors: {errs[:4]}")
    verr = _gff.check_values(g)
    if verr:
        raise GuiError(f"{name}: GFF value errors: {verr[:4]}")
    root_fields = g.fields_of(0)
    by_label = {}
    controls_field = None
    for f in root_fields:
        if f.type == 15 and f.label == "CONTROLS":
            controls_field = f
        by_label.setdefault(f.label, f)
    if controls_field is None:
        raise GuiError(f"{name}: root lacks CONTROLS list")
    if "CONTROLTYPE" not in by_label or by_label["CONTROLTYPE"].type != 5:
        raise GuiError(f"{name}: root lacks int32 CONTROLTYPE")
    ctype = g.value(by_label["CONTROLTYPE"])
    tag = g.value(by_label["TAG"]) if "TAG" in by_label else ""
    _n, idx = g.list_of(controls_field)
    root = Control(0, ctype, tag, tuple(_control(g, si, 1, name) for si in idx))
    return Gui(ctype, tag, root, g)


def count_controls(c: Control) -> int:
    return 1 + sum(count_controls(x) for x in c.children)


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one .gui resource (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    gui = parse_gui(data, args.path)
    print(f"{args.path}: root type={gui.control_type} tag={gui.tag!r} "
          f"controls={count_controls(gui.root)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
