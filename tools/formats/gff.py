#!/usr/bin/env python3
"""Read-only GFF3 probe for KotOR coordinate research.

Wire layout implemented (see docs/recon/03_GFF_COORDINATES.md for evidence):

  header  : magic[4] + version[4] + 12 x u32
            (struct off/cnt, field off/cnt, label off/cnt,
             field-data off/byte-len, field-index off/byte-len,
             list-index off/byte-len)
  struct  : u32 type_id, u32 data_or_offset, u32 field_count
            field_count == 1  -> data_or_offset is a DIRECT field-table row
            field_count >  1  -> data_or_offset is a BYTE offset into field_indices
  field   : u32 field_type, u32 label_index, u32 data_or_data_offset
            (type at +0 per bioware_gff_common.ksy lines 22 and 71-170;
             verified against every field in both sample files)
  multi-field structs are referenced indirectly: field-index table[slot] -> field row.
  label   : 16-byte NUL-padded ASCII, 16 bytes per entry
  list    : at list-data[offset]: u32 count, then count x u32 struct index

Read-only: opens paths 'rb' and never writes to them.

LANE 03 PATCH (proposal only; public tools/formats/gff.py is unchanged):
  * value(): explicit field-data bounds checks for types 6/7/9/10/11/12/13/16/17
    with byte offsets in the error text instead of bare struct.error.
  * CExoLocString: validates count/substring lengths and KotOR's
    total == 8 + substring-bytes convention.
  * list_of(): clean bounds errors.
  * check(): field-index slots need only be disjoint and in range. Shipped
    KotOR files order slots by creation, not struct index (root often last),
    and may carry stale unreferenced slot entries, so the old exact
    "packed in struct order / ends at field_rows" rule is a false positive.
  * check(): added struct->field->label contiguity checks.
  * check_values(): opt-in value resolution with per-field offsets.
  * as_pairs(): preserves duplicate record labels (shipped DLG quirk).
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
    __slots__ = ("index", "type_id", "slot", "count", "rows", "single")

    def __init__(self, index, type_id, slot, count, rows, single=False):
        self.index = index
        self.type_id = type_id
        self.slot = slot          # field-index slot; None for single-field structs
        self.count = count
        self.rows = rows          # field-table rows
        self.single = single      # field_count == 1: direct row, no slot


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
            if cnt == 0:
                rows, slot, single = [], None, False
            elif cnt == 1:
                if off >= self.field_cnt:
                    raise ValueError(f"struct {i}: direct field row {off} out of range")
                rows, slot, single = [off], None, True
            else:
                if off % 4:
                    raise ValueError(f"struct {i}: field-index offset {off} not u32 aligned")
                slot = off // 4
                if slot + cnt > len(self.field_rows):
                    raise ValueError(f"struct {i}: field slots {slot}+{cnt} overflow")
                rows = self.field_rows[slot:slot + cnt]
                for r in rows:
                    if r >= self.field_cnt:
                        raise ValueError(f"struct {i}: field row {r} out of range")
                single = False
            self.structs.append(Struct(i, type_id, slot, cnt, rows, single))

    # ---------- primitives ----------
    def _check_section(self, name, off, length):
        if off < 0 or length < 0 or off + length > len(self.buf):
            raise ValueError(f"{name} section {off}+{length} exceeds file {len(self.buf)}")

    def _cstr(self, off, maxlen):
        raw = self.buf[off:off + maxlen].split(b"\0")[0]
        return raw.decode("latin-1")

    def _need(self, off, length, what):
        if off < 0 or length < 0 or off + length > len(self.fdata):
            raise ValueError(
                f"{what}: needs {length} bytes at field-data+{off}, "
                f"field-data length {len(self.fdata)}")
        return self.fdata

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
            self._need(d, 8, "uint64")
            return struct.unpack_from("<Q", self.fdata, d)[0]
        if t == 7:
            self._need(d, 8, "int64")
            return struct.unpack_from("<q", self.fdata, d)[0]
        if t == 8:
            return struct.unpack("<f", struct.pack("<I", d))[0]
        if t == 9:
            self._need(d, 8, "double")
            return struct.unpack_from("<d", self.fdata, d)[0]
        if t == 10:  # CExoString: u32 byte count, no terminator required
            self._need(d, 4, "string length")
            n = struct.unpack_from("<I", self.fdata, d)[0]
            self._need(d + 4, n, "string bytes")
            return self.fdata[d + 4:d + 4 + n].split(b"\0")[0].decode("latin-1")
        if t == 11:  # ResRef: u8 byte count, no terminator required
            self._need(d, 1, "resref length")
            n = self.fdata[d]
            self._need(d + 1, n, "resref bytes")
            return self.fdata[d + 1:d + 1 + n].decode("latin-1")
        if t == 12:  # CExoLocString
            self._need(d, 12, "locstring header")
            total, strref, count = struct.unpack_from("<3I", self.fdata, d)
            if count > (len(self.fdata) - (d + 12)) // 8:
                raise ValueError(f"locstring count {count} overruns field data")
            out = {"total": total, "strref": strref, "substrings": []}
            p = d + 12
            for i in range(count):
                self._need(p, 8, f"locstring substring {i} header")
                lang, n = struct.unpack_from("<2I", self.fdata, p)
                p += 8
                self._need(p, n, f"locstring substring {i} bytes")
                out["substrings"].append(
                    (lang, self.fdata[p:p + n].split(b"\0")[0].decode("latin-1")))
                p += n
            # KotOR writes total = 8 + substring bytes (it excludes itself).
            actual = 8 + (p - (d + 12))
            if total != actual:
                raise ValueError(
                    f"locstring total {total} != expected {actual} "
                    f"(8 + substring bytes)")
            return out
        if t == 13:  # binary: u32 length, bytes
            self._need(d, 4, "binary length")
            n = struct.unpack_from("<I", self.fdata, d)[0]
            self._need(d + 4, n, "binary bytes")
            return self.fdata[d + 4:d + 4 + n]
        if t == 14:
            return {"__struct__": d}
        if t == 15:
            return {"__list__": d}
        if t == 16:
            self._need(d, 16, "vector4")
            return struct.unpack_from("<4f", self.fdata, d)
        if t == 17:
            self._need(d, 12, "vector3")
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

    def as_pairs(self, sindex):
        """Label/value pairs preserving duplicate record labels (DLG quirk)."""
        return [(f.label, self.value(f)) for f in self.fields_of(sindex)]

    def list_of(self, f):
        """Return (count, struct_indices) for a LIST field."""
        if f.type != 15:
            raise ValueError(f"{f.label} is not a list")
        off = f.data
        if off < 0 or off + 4 > len(self.lidx):
            raise ValueError(f"{f.label}: list offset {off} outside list-index")
        n = struct.unpack_from("<I", self.lidx, off)[0]
        if off + 4 + 4 * n > len(self.lidx):
            raise ValueError(f"{f.label}: {n} entries overrun list-index")
        idx = list(struct.unpack_from("<%dI" % n, self.lidx, off + 4)) if n else []
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
    # 2. multi-field structs own disjoint field-index slot ranges within the
    #    table. Shipped KotOR files order ranges by creation, not struct index
    #    (the root is often written last) and may contain stale unreferenced
    #    entries, so packing/order/full-coverage are NOT invariants.
    ranges = []
    for s in g.structs:
        if s.single or s.count == 0:
            continue
        if s.slot is None or s.slot * 4 + s.count * 4 > g.fidx_len:
            errs.append(f"struct {s.index} slot range {s.slot}+{s.count} "
                        f"outside field-index bytes {g.fidx_len}")
            continue
        ranges.append((s.slot, s.slot + s.count, s.index))
    ranges.sort()
    pos = 0
    for a, z, si in ranges:
        if a < pos:
            errs.append(f"struct {si} slots {a}:{z} overlap previous range")
        pos = max(pos, z)
    # 3. tables are contiguous and in header order (header is 56 bytes).
    if g.struct_off != 56:
        errs.append(f"struct table starts at {g.struct_off}, expected 56")
    if g.field_off != g.struct_off + g.struct_cnt * 12:
        errs.append("struct table does not end at field table start")
    if g.label_off != g.field_off + g.field_cnt * 12:
        errs.append("field table does not end at label table start")
    if g.fidx_len % 4:
        errs.append(f"field-index byte length {g.fidx_len} not a multiple of 4")
    if g.lidx_len % 4:
        errs.append(f"list-index byte length {g.lidx_len} not a multiple of 4")
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


def check_values(g):
    """Resolve every field value; return [(offset, message)] with file offsets."""
    out = []
    offset_types = {6, 7, 9, 10, 11, 12, 13, 16, 17}
    for s in g.structs:
        for f in g.fields_of(s.index):
            try:
                g.value(f)
                if f.type == 15:
                    g.list_of(f)
            except Exception as exc:
                where = (g.fdata_off + f.data if f.type in offset_types
                         else g.field_off + f.row * 12)
                out.append((where, f"struct {s.index} {f.label}: "
                                  f"{type(exc).__name__}: {exc}"))
    return out


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
    s0 = g.structs[0]
    where = f"single_row={s0.rows[0]}" if s0.single else f"slots={s0.slot}"
    print(f"type_id=0x{s0.type_id:08x} {where} count={s0.count}")
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
    where = f" single_row={s.rows[0]}" if s.single else ""
    print(f"{indent}struct[{si}] type_id=0x{s.type_id:08x} fields={s.count}{where}")
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
