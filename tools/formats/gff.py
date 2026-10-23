#!/usr/bin/env python3
"""Read-only GFF3 probe for KotOR coordinate research.

Wire layout implemented (see docs/recon/03_GFF_COORDINATES.md for evidence):

  header  : magic[4] + version[4] + 12 x u32
            (struct off/cnt, field off/cnt, label off/cnt,
             field-data off/byte-len, field-index off/byte-len,
             list-index off/byte-len)
  struct  : u32 type_id, u32 field_index_byte_offset, u32 field_count
  field   : u32 field_type, u32 label_index, u32 data_or_data_offset
            (type at +0 per bioware_gff_common.ksy lines 22 and 71-170;
             verified against every field in both sample files)
  fields are referenced indirectly: field-index table[slot] -> field row.
  label   : 16-byte NUL-padded ASCII, 16 bytes per entry
  list    : at list-data[offset]: u32 count, then count x u32 struct index

Read-only: opens paths 'rb' and never writes to them.
"""
from __future__ import annotations

import argparse
import struct
import sys

TYPES = {
    0: "uint8", 1: "int8", 2: "uint16", 3: "int16", 4: "uint32",
    5: "int32", 6: "uint64", 7: "int64", 8: "float", 9: "double",
    10: "string", 11: "resref", 12: "locstring", 13: "binary",
    14: "struct", 15: "list", 16: "vector4", 17: "vector3", 18: "strref",
}

INLINE = {0, 1, 2, 3, 4, 5, 8, 18}


class Field:
    __slots__ = ("row", "type", "label_index", "data", "label")

    def __init__(self, row, ftype, label_index, data, label):
        self.row = row
        self.type = ftype
        self.label_index = label_index
        self.data = data
        self.label = label

    def __repr__(self):
        return f"Field(row={self.row}, {self.label!r}, type={self.type}, data=0x{self.data:08x})"


class Struct:
    __slots__ = ("index", "type_id", "slot", "count", "rows")

    def __init__(self, index, type_id, slot, count, rows):
        self.index = index
        self.type_id = type_id
        self.slot = slot          # field-index slot (byte offset // 4)
        self.count = count
        self.rows = rows          # field-table row per slot


class Gff:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.buf = f.read()
        b = self.buf
        if len(b) < 56:
            raise ValueError("file shorter than 56-byte GFF header")
        self.magic = b[0:4]
        self.version = b[4:8]
        (self.struct_off, self.struct_cnt, self.field_off, self.field_cnt,
         self.label_off, self.label_cnt, self.fdata_off, self.fdata_len,
         self.fidx_off, self.fidx_len, self.lidx_off, self.lidx_len) = \
            struct.unpack_from("<12I", b, 8)

        self._check_section("struct", self.struct_off, self.struct_cnt * 12)
        self._check_section("field", self.field_off, self.field_cnt * 12)
        self._check_section("label", self.label_off, self.label_cnt * 16)
        self._check_section("field-data", self.fdata_off, self.fdata_len)
        self._check_section("field-index", self.fidx_off, self.fidx_len)
        self._check_section("list-index", self.lidx_off, self.lidx_len)

        self.labels = [self._cstr(self.label_off + i * 16, 16)
                       for i in range(self.label_cnt)]
        self.fdata = b[self.fdata_off:self.fdata_off + self.fdata_len]
        self.lidx = b[self.lidx_off:self.lidx_off + self.lidx_len]
        self.field_rows = list(struct.unpack_from(
            "<%dI" % (self.fidx_len // 4), b, self.fidx_off))

        self.fields = []
        for row in range(self.field_cnt):
            ftype, lab, data = struct.unpack_from("<3I", b, self.field_off + row * 12)
            if ftype > 18:
                raise ValueError(f"field row {row}: invalid type {ftype}")
            if lab >= self.label_cnt:
                raise ValueError(f"field row {row}: label {lab} out of range")
            self.fields.append(Field(row, ftype, lab, data, self.labels[lab]))

        self.structs = []
        for i in range(self.struct_cnt):
            type_id, off, cnt = struct.unpack_from("<3I", b, self.struct_off + i * 12)
            if off % 4:
                raise ValueError(f"struct {i}: field-index offset {off} not u32 aligned")
            slot = off // 4
            if slot + cnt > len(self.field_rows):
                raise ValueError(f"struct {i}: field slots {slot}+{cnt} overflow")
            rows = self.field_rows[slot:slot + cnt]
            for r in rows:
                if r >= self.field_cnt:
                    raise ValueError(f"struct {i}: field row {r} out of range")
            self.structs.append(Struct(i, type_id, slot, cnt, rows))

    # ---------- primitives ----------
    def _check_section(self, name, off, length):
        if off < 0 or length < 0 or off + length > len(self.buf):
            raise ValueError(f"{name} section {off}+{length} exceeds file {len(self.buf)}")

    def _cstr(self, off, maxlen):
        raw = self.buf[off:off + maxlen].split(b"\0")[0]
        return raw.decode("latin-1")

    # ---------- value resolution ----------
    def value(self, f):
        t, d = f.type, f.data
        if t == 0:
            return d & 0xFF
        if t == 1:
            return struct.unpack("<b", bytes([d & 0xFF]))[0]
        if t == 2:
            return d & 0xFFFF
        if t == 3:
            return struct.unpack("<h", struct.pack("<H", d & 0xFFFF))[0]
        if t == 4:
            return d
        if t == 5:
            return struct.unpack("<i", struct.pack("<I", d))[0]
        if t == 6:
            return struct.unpack_from("<Q", self.fdata, d)[0]
        if t == 7:
            return struct.unpack_from("<q", self.fdata, d)[0]
        if t == 8:
            return struct.unpack("<f", struct.pack("<I", d))[0]
        if t == 9:
            return struct.unpack_from("<d", self.fdata, d)[0]
        if t == 10:  # CExoString: u32 length incl. NUL, bytes
            n = struct.unpack_from("<I", self.fdata, d)[0]
            return self.fdata[d + 4:d + 4 + n].split(b"\0")[0].decode("latin-1")
        if t == 11:  # ResRef: u8 length, bytes
            n = self.fdata[d]
            return self.fdata[d + 1:d + 1 + n].decode("latin-1")
        if t == 12:  # CExoLocString
            total, strref, count = struct.unpack_from("<3I", self.fdata, d)
            out = {"strref": strref, "substrings": []}
            p = d + 12
            for _ in range(count):
                lang, n = struct.unpack_from("<2I", self.fdata, p)
                p += 8
                out["substrings"].append(
                    (lang, self.fdata[p:p + n].split(b"\0")[0].decode("latin-1")))
                p += n
            return out
        if t == 13:  # binary: u32 length, bytes
            n = struct.unpack_from("<I", self.fdata, d)[0]
            return self.fdata[d + 4:d + 4 + n]
        if t == 14:
            return {"__struct__": d}
        if t == 15:
            return {"__list__": d}
        if t == 16:
            return struct.unpack_from("<4f", self.fdata, d)
        if t == 17:
            return struct.unpack_from("<3f", self.fdata, d)
        if t == 18:
            return d
        raise ValueError(f"unhandled type {t}")

    def struct_type_id(self, sindex):
        return self.structs[sindex].type_id

    def fields_of(self, sindex):
        s = self.structs[sindex]
        return [self.fields[r] for r in s.rows]

    def as_dict(self, sindex):
        out = {}
        for f in self.fields_of(sindex):
            v = self.value(f)
            if isinstance(v, dict) and "__struct__" in v:
                v = {"__struct__": v["__struct__"]}
            out[f.label] = v
        return out

    def list_of(self, f):
        """Return (count, struct_indices) for a LIST field."""
        if f.type != 15:
            raise ValueError(f"{f.label} is not a list")
        off = f.data
        n = struct.unpack_from("<I", self.lidx, off)[0]
        idx = list(struct.unpack_from("<%dI" % n, self.lidx, off + 4))
        return n, idx

    def describe(self):
        return (f"{self.path}: magic={self.magic!r} version={self.version!r} "
                f"structs={self.struct_cnt} fields={self.field_cnt} "
                f"labels={self.label_cnt} field_data={self.fdata_len}B "
                f"field_idx={self.fidx_len}B list_idx={self.lidx_len}B "
                f"size={len(self.buf)}B")


def check(g):
    """Structural invariants; each must hold for the parse to be trusted."""
    errs = []
    # 1. every field row is referenced by exactly one struct slot
    refs = [0] * g.field_cnt
    for s in g.structs:
        for r in s.rows:
            refs[r] += 1
    if any(c != 1 for c in refs):
        errs.append("field rows not a permutation: "
                    + str([(i, c) for i, c in enumerate(refs) if c != 1][:8]))
    # 2. struct field slots are packed in struct order
    slot = 0
    for s in g.structs:
        if s.slot != slot:
            errs.append(f"struct {s.index} slot {s.slot} != expected {slot}")
        slot += s.count
    if slot != len(g.field_rows):
        errs.append(f"struct slots end {slot} != field-index entries {len(g.field_rows)}")
    # 3. sections are contiguous and in header order
    if g.label_off + g.label_cnt * 16 != g.fdata_off:
        errs.append("label section does not end at field-data start")
    if g.fdata_off + g.fdata_len != g.fidx_off:
        errs.append("field-data does not end at field-index start")
    if g.fidx_off + g.fidx_len != g.lidx_off:
        errs.append("field-index does not end at list-index start")
    if g.lidx_off + g.lidx_len != len(g.buf):
        errs.append("list-index does not end at EOF")
    # 4. list offsets and entries are in bounds
    seen = []
    for si in range(g.struct_cnt):
        for f in g.fields_of(si):
            if f.type != 15:
                continue
            if f.data + 4 > g.lidx_len:
                errs.append(f"struct {si} {f.label}: list offset {f.data} out of range")
                continue
            n, idx = g.list_of(f)
            if f.data + 4 + 4 * n > g.lidx_len:
                errs.append(f"struct {si} {f.label}: {n} entries overflow list data")
            for x in idx:
                if x >= g.struct_cnt:
                    errs.append(f"struct {si} {f.label}: struct index {x} out of range")
            seen.append((f.data, f.data + 4 + 4 * n, f.label))
    # 5. list entries pack without overlap
    seen.sort()
    for (a0, a1, la), (b0, b1, lb) in zip(seen, seen[1:]):
        if b0 < a1:
            errs.append(f"list ranges overlap: {la} {a0}:{a1} vs {lb} {b0}:{b1}")
    return errs


def fmt(v):
    if isinstance(v, float):
        return f"{v:.6f}"
    if isinstance(v, tuple):
        return "(" + ", ".join(f"{x:.6f}" if isinstance(x, float) else str(x) for x in v) + ")"
    if isinstance(v, bytes):
        return f"<{len(v)}B>"
    return str(v)


def dump_root(g):
    print("\n== root struct 0 ==")
    print(f"type_id=0x{g.structs[0].type_id:08x} slots={g.structs[0].slot} "
          f"count={g.structs[0].count}")
    for f in g.fields_of(0):
        v = g.value(f)
        if f.type == 15:
            n, idx = g.list_of(f)
            print(f"  [{f.row:3d}] {f.label:<18} type=list  list_off={f.data:4d} "
                  f"count={n} structs={idx}")
        elif f.type == 14:
            print(f"  [{f.row:3d}] {f.label:<18} type=struct struct={v['__struct__']} "
                  f"(type_id=0x{g.structs[v['__struct__']].type_id:08x})")
        else:
            print(f"  [{f.row:3d}] {f.label:<18} type={TYPES[f.type]:<10} {fmt(v)}")


def show_struct(g, si, indent="    "):
    s = g.structs[si]
    print(f"{indent}struct[{si}] type_id=0x{s.type_id:08x} fields={s.count}")
    for f in g.fields_of(si):
        v = g.value(f)
        if f.type == 15:
            n, idx = g.list_of(f)
            print(f"{indent}  {f.label:<20} list count={n} structs={idx}")
        elif f.type == 14:
            print(f"{indent}  {f.label:<20} struct={v['__struct__']} "
                  f"(type_id=0x{g.structs[v['__struct__']].type_id:08x})")
        else:
            print(f"{indent}  {f.label:<20} {TYPES[f.type]:<10} {fmt(v)}")


COORD_KEYS = ("XPosition", "YPosition", "ZPosition", "XOrientation",
              "YOrientation", "Bearing", "X", "Y", "Z")


def entries(g, root=0):
    """Yield (list_label, ordinal, struct_index, dict) for every list instance."""
    for f in g.fields_of(root):
        if f.type != 15:
            continue
        n, idx = g.list_of(f)
        for k, si in enumerate(idx):
            yield f.label, k, si, g.as_dict(si)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Read-only GFF3 coordinate probe")
    ap.add_argument("path")
    ap.add_argument("--root", action="store_true", help="print root struct + lists")
    ap.add_argument("--entries", action="store_true", help="print every list instance")
    ap.add_argument("--struct", type=int, help="dump one struct index")
    ap.add_argument("--all-structs", action="store_true", help="dump every struct")
    ap.add_argument("--list", dest="list_name", help="dump one list by label")
    ap.add_argument("--check", action="store_true", help="run structural invariants")
    a = ap.parse_args(argv)

    g = Gff(a.path)
    print(g.describe())
    if a.check:
        errs = check(g)
        print("structural check: " + ("PASS" if not errs else f"{len(errs)} FAILURES"))
        for e in errs:
            print("  !", e)
    if a.root or not (a.entries or a.all_structs or a.list_name is not None):
        dump_root(g)
    if a.struct is not None:
        show_struct(g, a.struct)
    if a.all_structs:
        for si in range(g.struct_cnt):
            show_struct(g, si)
    if a.list_name is not None:
        for f in g.fields_of(0):
            if f.type == 15 and f.label == a.list_name:
                n, idx = g.list_of(f)
                print(f"\n== {f.label}: count={n} structs={idx} ==")
                for si in idx:
                    show_struct(g, si)
    if a.entries:
        print("\n== list instances ==")
        for lab, k, si, d in entries(g):
            coords = {key: d[key] for key in COORD_KEYS if key in d}
            tmpl = d.get("TemplateResRef", d.get("TemplateResref"))
            print(f"  {lab}[{k}] -> struct[{si}] type_id=0x{g.structs[si].type_id:08x} "
                  f"TemplateResRef={tmpl!r}")
            if coords:
                print("      " + ", ".join(f"{k}={fmt(v)}" for k, v in coords.items()))
            else:
                print("      (no coordinate fields)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
