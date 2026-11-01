"""Minimal .gui (GFF V3.2, type 2047) parser: control tree + texture refs.

Verified against shipped screens (e.g. ``mainmenu`` from ``data/gui.bif``,
build SHA-256 0290889e...74e6): the root struct carries a ``CONTROLS`` list;
each control is a flat struct with ``CONTROLTYPE`` (int32), ``TAG`` (string),
``EXTENT`` (struct LEFT/TOP/WIDTH/HEIGHT in 640x480 GUI space), ``BORDER`` /
``HILIGHT`` (structs with CORNER/EDGE/FILL resrefs), ``TEXT`` (FONT resref),
plus type-specific structs (PROTOITEM, SCROLLBAR with DIR/THUMB IMAGE
resrefs, ...).  Controls are a flat list; stacking order is list order.
"""

from __future__ import annotations

from . import gffx

gff = gffx.gff

CONTROL_TYPE_NAMES = {
    0: "label?", 1: "?", 2: "panel", 3: "?", 4: "image/label", 5: "?",
    6: "button", 7: "?", 8: "?", 9: "scrollbar", 10: "?", 11: "listbox",
    12: "?", 13: "?", 14: "?", 15: "?", 16: "?",
}

TEXTURE_LABELS = {"CORNER", "EDGE", "FILL", "IMAGE", "FONT"}


def _walk_resrefs(g, sindex: int, out: list, depth: int = 0):
    if depth > 6:
        return
    for f in g.fields_of(sindex):
        v = g.value(f)
        if f.type == 11 and f.label in TEXTURE_LABELS and v:
            out.append({"label": f.label, "resref": v})
        elif f.type == 14 and isinstance(v, dict) and "__struct__" in v:
            _walk_resrefs(g, v["__struct__"], out, depth + 1)
        elif f.type == 15:
            _, idx = g.list_of(f)
            for si in idx:
                _walk_resrefs(g, si, out, depth + 1)


def parse_gui(data: bytes, name: str = "gui") -> dict:
    g = gffx.gff_from_bytes(data, name + ".gui")
    root = g.as_dict(0)
    controls = []
    for f in g.fields_of(0):
        if f.type == 15 and f.label == "CONTROLS":
            n, idx = g.list_of(f)
            for si in idx:
                d = g.as_dict(si)
                extent = gffx.struct_field(g, si, "EXTENT") or {}
                border = gffx.struct_field(g, si, "BORDER") or {}
                hilight = gffx.struct_field(g, si, "HILIGHT") or {}
                text = gffx.struct_field(g, si, "TEXT") or {}
                refs: list[dict] = []
                _walk_resrefs(g, si, refs)
                controls.append({
                    "tag": d.get("TAG", ""),
                    "control_type": d.get("CONTROLTYPE"),
                    "control_type_name": CONTROL_TYPE_NAMES.get(d.get("CONTROLTYPE"), "?"),
                    "id": d.get("ID"),
                    "extent": {k: extent.get(k) for k in ("LEFT", "TOP", "WIDTH", "HEIGHT")},
                    "border_fill": border.get("FILL", ""),
                    "border_edge": border.get("EDGE", ""),
                    "border_corner": border.get("CORNER", ""),
                    "hilight_fill": hilight.get("FILL", ""),
                    "font": text.get("FONT", ""),
                    "text": text.get("TEXT", ""),
                    "texture_refs": refs,
                })
            break
    return {
        "name": name,
        "gff": g,
        "root_fields": {f.label: (f.type, gff.fmt(g.value(f))[:60])
                        for f in g.fields_of(0) if f.type != 15},
        "controls": controls,
        "control_count": len(controls),
        "checks": [{"name": "gff_check", "ok": gff.check(g) == [],
                    "errors": gff.check(g)}],
    }
