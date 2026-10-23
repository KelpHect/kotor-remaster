#!/usr/bin/env python3
"""GFF V3.2 writer and semantic re-serializer for lane 05 fixtures.

The reader is the project's verified ``tools/formats/gff.py``.  This module
does not modify it; it imports it read-only and produces bytes that satisfy
that reader's structural checks.

Scope and limits:
* Real KotOR GFF files use the layout verified in docs/recon/03.  The writer
  emits the same section order and the single-field-struct rule.
* The writer recomputes all offsets and section sizes.  It does not preserve
  unused labels, original row order, or padding, so rewritten files are
  semantically equal, not byte-identical.
* The writer is deliberately strict about labels/resrefs/strings so a fixture
  cannot silently become an invalid game file.

Usage:
  python kotor_gff_write.py roundtrip IN.gff OUT.gff
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GFF_DIR = REPO_ROOT / "tools" / "formats"
if str(GFF_DIR) not in sys.path:
    sys.path.insert(0, str(GFF_DIR))
import gff  # noqa: E402  (project parser, read-only import)


TYPE_IDS = {name: tid for tid, name in gff.TYPES.items()}
INLINE_TYPES = {0, 1, 2, 3, 4, 5, 8, 18}
FDATA_TYPES = {6, 7, 9, 10, 11, 12, 13, 16, 17}


class GffWriteError(ValueError):
    pass


class GffStruct:
    """Minimal GFF struct node.  Fields are kept in insertion order."""

    __slots__ = ("type_id", "fields")

    def __init__(self, type_id: int = 0):
        self.type_id = int(type_id) & 0xFFFFFFFF
        self.fields: list[tuple[str, int, object]] = []

    def add(self, label: str, ftype: int | str, value) -> "GffStruct":
        if isinstance(ftype, str):
            if ftype not in TYPE_IDS:
                raise GffWriteError(f"unknown GFF type {ftype!r}")
            ftype = TYPE_IDS[ftype]
        if not 0 <= int(ftype) <= 18:
            raise GffWriteError(f"invalid GFF type id {ftype}")
        self.fields.append((str(label), int(ftype), value))
        return self

    def add_many(self, *rows) -> "GffStruct":
        for label, ftype, value in rows:
            self.add(label, ftype, value)
        return self

    def __repr__(self) -> str:
        return f"GffStruct(type_id=0x{self.type_id:08x}, fields={len(self.fields)})"


def _encode_label(label: str) -> bytes:
    raw = label.encode("latin-1")
    if b"\x00" in raw:
        raise GffWriteError("label contains NUL")
    if len(raw) > 16:
        raise GffWriteError(f"label {label!r} longer than 16 bytes")
    return raw.ljust(16, b"\x00")


def _text(value, field: str) -> bytes:
    if not isinstance(value, str):
        raise GffWriteError(f"{field} must be str")
    try:
        return value.encode("latin-1")
    except UnicodeEncodeError as exc:
        raise GffWriteError(f"{field} is not latin-1 encodable") from exc


def _resref(value, field: str) -> bytes:
    raw = _text(value, field)
    if b"\x00" in raw:
        raise GffWriteError(f"{field} contains NUL")
    if len(raw) > 16:
        raise GffWriteError(f"{field} resref longer than 16 bytes")
    return raw


def _collect_structs(root: GffStruct) -> list[GffStruct]:
    order: list[GffStruct] = []
    seen: set[int] = set()

    def visit(node: GffStruct):
        ident = id(node)
        if ident in seen:
            raise GffWriteError("shared GffStruct instance is not representable")
        seen.add(ident)
        order.append(node)
        for label, ftype, value in node.fields:
            if ftype == 14:
                if not isinstance(value, GffStruct):
                    raise GffWriteError(f"{label}: struct field needs GffStruct")
                visit(value)
            elif ftype == 15:
                if not isinstance(value, (list, tuple)):
                    raise GffWriteError(f"{label}: list field needs a sequence")
                for child in value:
                    if not isinstance(child, GffStruct):
                        raise GffWriteError(f"{label}: list item needs GffStruct")
                    visit(child)

    visit(root)
    return order


def gff_bytes(file_type: str | bytes, root: GffStruct,
              version: str | bytes = b"V3.2") -> bytes:
    """Serialize ``root`` and return GFF V3.2 bytes."""
    if not isinstance(root, GffStruct):
        raise GffWriteError("root must be GffStruct")
    ft = file_type.encode("latin-1") if isinstance(file_type, str) else bytes(file_type)
    ver = version.encode("latin-1") if isinstance(version, str) else bytes(version)
    if len(ft) != 4:
        raise GffWriteError("GFF file type must be exactly 4 bytes")
    if len(ver) != 4:
        raise GffWriteError("GFF version must be exactly 4 bytes")

    structs = _collect_structs(root)
    struct_index = {id(s): i for i, s in enumerate(structs)}

    labels: list[bytes] = []
    label_index: dict[str, int] = {}

    def label_id(label: str) -> int:
        if label not in label_index:
            label_index[label] = len(labels)
            labels.append(_encode_label(label))
        return label_index[label]

    fdata = bytearray()
    fidx = bytearray()
    lidx = bytearray()
    field_rows: list[tuple[int, int, int]] = []
    struct_rows: list[tuple[int, int, int]] = []

    def fdata_add(payload: bytes) -> int:
        while len(fdata) % 4:
            fdata.append(0)
        offset = len(fdata)
        fdata.extend(payload)
        return offset

    def encode(ftype: int, value, field: str) -> int:
        if ftype in INLINE_TYPES:
            if ftype in (0, 2, 4, 18):
                if not isinstance(value, int) or isinstance(value, bool):
                    # bool is a legitimate uint8/uint32 source; normalize.
                    if isinstance(value, bool) and ftype in (0, 2, 4, 18):
                        value = int(value)
                    else:
                        raise GffWriteError(f"{field}: expected int for inline type")
                if not 0 <= value <= 0xFFFFFFFF:
                    raise GffWriteError(f"{field}: inline value out of u32 range")
                return value
            if ftype in (1, 3):
                if not isinstance(value, int) or isinstance(value, bool):
                    raise GffWriteError(f"{field}: expected int")
                bits = 8 if ftype == 1 else 16
                return int(value) & ((1 << bits) - 1)
            if ftype == 5:
                if not isinstance(value, int) or isinstance(value, bool):
                    raise GffWriteError(f"{field}: expected int")
                return int(value) & 0xFFFFFFFF
            if ftype == 8:
                return struct.unpack("<I", struct.pack("<f", float(value)))[0]
        if ftype == 6:
            return fdata_add(struct.pack("<Q", int(value) & 0xFFFFFFFFFFFFFFFF))
        if ftype == 7:
            return fdata_add(struct.pack("<q", int(value)))
        if ftype == 9:
            return fdata_add(struct.pack("<d", float(value)))
        if ftype == 10:
            raw = _text(value, field)
            # Retail CExoString length excludes a terminator; the verified
            # reader accepts both forms but this matches the corpus.
            return fdata_add(struct.pack("<I", len(raw)) + raw)
        if ftype == 11:
            raw = _resref(value, field)
            return fdata_add(bytes([len(raw)]) + raw)
        if ftype == 12:
            if not isinstance(value, dict):
                raise GffWriteError(f"{field}: locstring needs a dict")
            strref = int(value.get("strref", 0xFFFFFFFF)) & 0xFFFFFFFF
            subs = value.get("substrings", [])
            if not isinstance(subs, (list, tuple)):
                raise GffWriteError(f"{field}: substrings must be a sequence")
            payload = bytearray(struct.pack("<3I", 0, strref, len(subs)))
            substring_bytes = 0
            for item in subs:
                if not isinstance(item, (list, tuple)) or len(item) != 2:
                    raise GffWriteError(f"{field}: substring must be (lang, text)")
                lang, text = item
                raw = _text(text, f"{field} substring")
                payload += struct.pack("<2I", int(lang) & 0xFFFFFFFF, len(raw))
                payload += raw
                substring_bytes += 8 + len(raw)
            # Retail KotOR writes total = 8 + substring bytes; it excludes
            # the 12-byte (total,strref,count) header.  tools/formats/gff.py
            # enforces this, so do the same.
            struct.pack_into("<I", payload, 0, 8 + substring_bytes)
            return fdata_add(bytes(payload))
        if ftype == 13:
            if not isinstance(value, (bytes, bytearray)):
                raise GffWriteError(f"{field}: binary field needs bytes")
            return fdata_add(struct.pack("<I", len(value)) + bytes(value))
        if ftype == 14:
            if not isinstance(value, GffStruct):
                raise GffWriteError(f"{field}: struct field needs GffStruct")
            return struct_index[id(value)]
        if ftype == 15:
            if not isinstance(value, (list, tuple)):
                raise GffWriteError(f"{field}: list field needs a sequence")
            offset = len(lidx)
            lidx.extend(struct.pack("<I", len(value)))
            for child in value:
                lidx.extend(struct.pack("<I", struct_index[id(child)]))
            return offset
        if ftype == 16:
            vals = [float(x) for x in value]
            if len(vals) != 4:
                raise GffWriteError(f"{field}: vector4 needs 4 floats")
            return fdata_add(struct.pack("<4f", *vals))
        if ftype == 17:
            vals = [float(x) for x in value]
            if len(vals) != 3:
                raise GffWriteError(f"{field}: vector3 needs 3 floats")
            return fdata_add(struct.pack("<3f", *vals))
        raise GffWriteError(f"{field}: unsupported GFF type {ftype}")

    for node in structs:
        rows: list[int] = []
        for label, ftype, value in node.fields:
            data = encode(ftype, value, label)
            rows.append(len(field_rows))
            field_rows.append((ftype, label_id(label), data))
        if not rows:
            struct_rows.append((node.type_id, 0, 0))
        elif len(rows) == 1:
            struct_rows.append((node.type_id, rows[0], 1))
        else:
            slot_offset = len(fidx)
            for row in rows:
                fidx.extend(struct.pack("<I", row))
            struct_rows.append((node.type_id, slot_offset, len(rows)))

    label_bytes = b"".join(labels)
    struct_bytes = b"".join(struct.pack("<3I", *row) for row in struct_rows)
    field_bytes = b"".join(struct.pack("<3I", *row) for row in field_rows)

    struct_off = 56
    field_off = struct_off + len(struct_bytes)
    label_off = field_off + len(field_bytes)
    fdata_off = label_off + len(label_bytes)
    fidx_off = fdata_off + len(fdata)
    lidx_off = fidx_off + len(fidx)
    header = struct.pack("<12I", struct_off, len(struct_rows),
                         field_off, len(field_rows),
                         label_off, len(labels),
                         fdata_off, len(fdata),
                         fidx_off, len(fidx),
                         lidx_off, len(lidx))
    return ft + ver + header + struct_bytes + field_bytes + label_bytes + bytes(fdata) + bytes(fidx) + bytes(lidx)


def write_gff(path: str | os.PathLike, file_type: str | bytes,
              root: GffStruct, version: str | bytes = b"V3.2") -> str:
    """Serialize ``root`` and write it to ``path``.  Returns the path."""
    blob = gff_bytes(file_type, root, version)
    with open(path, "wb") as f:
        f.write(blob)
    return str(path)


def from_gff(g: "gff.Gff", struct_index: int = 0) -> GffStruct:
    """Convert a parsed GFF tree into writer nodes, preserving field types."""
    node = GffStruct(g.structs[struct_index].type_id)
    for f in g.fields_of(struct_index):
        if f.type == 14:
            value = from_gff(g, f.data)
        elif f.type == 15:
            _n, indices = g.list_of(f)
            value = [from_gff(g, idx) for idx in indices]
        else:
            value = g.value(f)
            if f.type == 12:
                # gff.py returns {"strref": int, "substrings": [(lang, str)]}
                value = {
                    "strref": int(value.get("strref", 0xFFFFFFFF)),
                    "substrings": [(int(lang), text)
                                   for lang, text in value.get("substrings", [])],
                }
        node.add(f.label, f.type, value)
    return node


def semantic(g: "gff.Gff", struct_index: int = 0):
    """Canonical semantic representation used by tests (floats bit-exact)."""
    out = {"__type_id__": g.structs[struct_index].type_id, "fields": []}
    for f in g.fields_of(struct_index):
        if f.type == 14:
            value = semantic(g, f.data)
        elif f.type == 15:
            _n, indices = g.list_of(f)
            value = [semantic(g, idx) for idx in indices]
        else:
            value = _canon_value(g.value(f))
        out["fields"].append([f.label, f.type, value])
    return out


def _canon_value(value):
    if isinstance(value, float):
        return {"float_bits": struct.pack("<d", value).hex()}
    if isinstance(value, dict):
        return {k: _canon_value(v) for k, v in sorted(value.items())}
    if isinstance(value, (list, tuple)):
        return [_canon_value(v) for v in value]
    if isinstance(value, (bytes, bytearray)):
        return {"bytes": bytes(value).hex()}
    return value


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="GFF V3.2 semantic re-serializer")
    sub = ap.add_subparsers(dest="cmd", required=True)
    rt = sub.add_parser("roundtrip", help="parse IN, write OUT, verify structure")
    rt.add_argument("inp")
    rt.add_argument("out")
    rt.add_argument("--check", action="store_true", default=True)
    args = ap.parse_args(argv)

    if args.cmd == "roundtrip":
        src = gff.Gff(args.inp)
        before_errors = gff.check(src)
        node = from_gff(src)
        write_gff(args.out, src.magic, node, src.version)
        dst = gff.Gff(args.out)
        after_errors = gff.check(dst)
        same = semantic(src) == semantic(dst)
        print(f"in : {src.describe()}")
        print(f"out: {dst.describe()}")
        print(f"semantic_equal={same} in_errors={len(before_errors)} "
              f"out_errors={len(after_errors)}")
        for err in after_errors:
            print("  !", err)
        return 0 if same and not after_errors else 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
