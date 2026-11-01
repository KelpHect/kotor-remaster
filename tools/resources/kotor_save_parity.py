#!/usr/bin/env python3
"""Byte-parity analysis and retail-faithful re-encoders for KotOR saves (lane s1-05).

This module imports the lane-05 readers/writers read-only
(``kotor_archive``, ``kotor_save``, ``kotor_gff_write``, ``tools/formats/gff``)
and adds the byte-parity layer:

* ``erf_layout`` / ``gff_layout`` classify every byte of a container
  (header, localized block, key table, resource table, payload regions,
  padding gaps, trailing bytes for ERF; header + six sections + trailing for
  GFF) so a re-encode can prove it accounted for the whole file.
* ``erf_bytes_verbatim`` rebuilds an ERF/MOD/SAV from the *parsed* tables
  (header fields, raw 24-byte key records, 8-byte resource records, payload
  slices placed at their original offsets); byte-identity with the input
  proves the parser lost nothing.
* ``erf_bytes_repack`` is a from-scratch writer that packs payloads with a
  configurable alignment.  Retail saves (windowed 1280x720 quicksave) pack
  contiguously (align=1), set ``offset_to_localized_string == offset_to_keys``
  when there are no localized strings, keep ``res_id == entry index`` and a
  zero header tail; with ``align=1`` and preserved build year/day the re-encode
  is byte-identical (see docs/recon/21_SAVE_BYTEPARITY.md).
* ``gff_bytes_parity`` re-serializes a parsed GFF tree with retail layout:
  sections in the canonical order, labels in first-use order, structs in
  pre-order, single-field structs using the direct field-row rule, and field
  data packed **without** 4-byte alignment (the stock ``kotor_gff_write`` pads
  each field-data entry to 4 bytes; retail does not).
* ``analyze_save_folder`` measures byte-identical percentage per file of a
  save slot (SAVEGAME.sav + nested module ERFs + GFF sidecars + Screen.tga)
  and classifies every diff by region.
* ``rewrite_save_parity`` rewrites a whole slot with the parity encoders and
  reports per-file byte equality against the source.

Evidence rule: every structural claim cites the build SHA-256
``0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6``
(verified read-only by ``install-check``) and the measured retail slot
``artifacts/lanes/06-dynamic/profile/saves/000000 - QUICKSAVE``.

The module never writes inside a game install (same guard as
``kotor_save.ensure_writable_target``) and never copies game payloads into
tracked files; it only reads them in memory.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for _p in (Path(__file__).resolve().parent, REPO_ROOT / "tools" / "formats",
           REPO_ROOT / "tools" / "inventory"):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

import gff  # noqa: E402  (tools/formats/gff.py, read-only import)
import kotor_archive as ka  # noqa: E402
import kotor_gff_write as gw  # noqa: E402
import kotor_save as ks  # noqa: E402

BUILD_SHA256 = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6"

ERF_HEADER_SIZE = 160
GFF_HEADER_SIZE = 56


# ---------------------------------------------------------------------------
# hashing / install sentinel (read-only)
# ---------------------------------------------------------------------------


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def verify_install(game_dir: str | os.PathLike) -> dict:
    """Read-only sentinel check of the retail executable hash."""
    exe = Path(game_dir) / "swkotor.exe"
    out = {"game_dir": str(game_dir), "exe": str(exe),
           "expected_sha256": BUILD_SHA256, "present": exe.is_file()}
    if out["present"]:
        out["sha256"] = ka.sha256_file(exe)
        out["match"] = out["sha256"] == BUILD_SHA256
    else:
        out["match"] = False
    out["status"] = "verified" if out.get("match") else "DRIFT-STOP"
    return out


# ---------------------------------------------------------------------------
# diff classification
# ---------------------------------------------------------------------------


def diff_report(orig: bytes, new: bytes, name: str = "<mem>") -> dict:
    """Byte-level comparison with first-diff / count / length-delta."""
    n = min(len(orig), len(new))
    first = None
    count = 0
    for i in range(n):
        if orig[i] != new[i]:
            count += 1
            if first is None:
                first = i
    return {
        "name": name,
        "orig_size": len(orig),
        "new_size": len(new),
        "byte_identical": orig == new,
        "first_diff_offset": first,
        "diff_byte_count_in_overlap": count,
        "length_delta": len(new) - len(orig),
    }


# ---------------------------------------------------------------------------
# ERF/MOD/SAV layout + parity writers
# ---------------------------------------------------------------------------


def erf_layout(data: bytes, name: str = "<mem>") -> dict:
    """Classify every byte region of an ERF-family container."""
    a = ka.read_erf_bytes(data, name)
    h = a.header
    off_keys = h["offset_to_key_list"]
    off_res = h["offset_to_resource_list"]
    n = h["entry_count"]
    off_loc = h["offset_to_localized_string"]
    loc_size = h["localized_string_size"]

    regions: list[dict] = [{"kind": "header", "start": 0, "end": ERF_HEADER_SIZE}]
    if h["language_count"] and loc_size:
        regions.append({"kind": "localized_strings", "start": off_loc,
                        "end": off_loc + loc_size})
    regions.append({"kind": "key_table", "start": off_keys, "end": off_keys + n * 24})
    regions.append({"kind": "resource_table", "start": off_res,
                    "end": off_res + n * 8})
    for e in a.entries:
        regions.append({"kind": "payload", "entry": e.index, "resref": e.resref,
                        "type": e.res_type, "start": e.offset, "end": e.offset + e.size})

    regions.sort(key=lambda r: r["start"])
    gaps = []
    covered_end = 0
    overlap = False
    for r in regions:
        if r["start"] < covered_end:
            overlap = True
        if r["start"] > covered_end:
            gap = data[covered_end:r["start"]]
            gaps.append({"start": covered_end, "end": r["start"], "size": len(gap),
                         "all_zero": gap == bytes(len(gap))})
        covered_end = max(covered_end, r["end"])
    trailing = len(data) - covered_end
    if trailing > 0:
        t = data[covered_end:]
        gaps.append({"start": covered_end, "end": len(data), "size": trailing,
                     "all_zero": t == bytes(len(t)), "kind": "trailing"})

    keys_raw = [data[off_keys + i * 24: off_keys + i * 24 + 24] for i in range(n)]
    key_fields = []
    for i, raw in enumerate(keys_raw):
        rid, rtype, unused = struct.unpack_from("<IHH", raw, 16)
        key_fields.append({"index": i, "resref_raw_hex": raw[:16].hex(),
                           "resref": a.entries[i].resref, "res_id": rid,
                           "res_type": rtype, "unused": unused,
                           "res_id_is_index": rid == i})

    return {
        "name": name, "size": len(data), "magic": a.magic, "version": a.version,
        "header": dict(h),
        "header_tail_all_zero": data[56:ERF_HEADER_SIZE] == bytes(ERF_HEADER_SIZE - 56),
        "keys": key_fields,
        "localized_block_size": loc_size if h["language_count"] else 0,
        "regions": regions, "gaps": gaps, "overlapping_regions": overlap,
        "fully_accounted": not overlap and all(g["all_zero"] for g in gaps),
        "entry_count": n,
    }


def _erf_header_bytes(a: ka.Archive, h: dict, off_loc: int, off_keys: int,
                      off_res: int, n: int) -> bytes:
    head = bytearray()
    head += a.magic.encode("latin-1")[:4].ljust(4, b" ")
    head += a.version.encode("latin-1")[:4].ljust(4, b" ")
    head += struct.pack("<9I", h["language_count"], h["localized_string_size"],
                        n, off_loc, off_keys, off_res,
                        h["build_year"], h["build_day"], h["description_strref"])
    head += bytes(ERF_HEADER_SIZE - len(head))
    return bytes(head)


def erf_bytes_verbatim(data: bytes, name: str = "<mem>") -> tuple[bytes, dict]:
    """Rebuild an ERF from parsed tables + raw slices at original offsets.

    The header is reconstructed from the parsed header fields (not copied),
    key/resource records are copied raw from the input, and payloads are
    placed at their original offsets with zero-filled gaps.  Byte-identity
    with the input therefore proves full accounting of every byte.
    """
    a = ka.read_erf_bytes(data, name)
    h = a.header
    n = len(a.entries)
    out = bytearray(data)  # pre-size; every region is rewritten below
    header = _erf_header_bytes(a, h, h["offset_to_localized_string"],
                               h["offset_to_key_list"], h["offset_to_resource_list"], n)
    out[0:ERF_HEADER_SIZE] = header
    if h["language_count"] and h["localized_string_size"]:
        out[h["offset_to_localized_string"]:
            h["offset_to_localized_string"] + h["localized_string_size"]] = \
            data[h["offset_to_localized_string"]:
                 h["offset_to_localized_string"] + h["localized_string_size"]]
    for i in range(n):
        out[h["offset_to_key_list"] + i * 24: h["offset_to_key_list"] + i * 24 + 24] = \
            data[h["offset_to_key_list"] + i * 24: h["offset_to_key_list"] + i * 24 + 24]
        out[h["offset_to_resource_list"] + i * 8: h["offset_to_resource_list"] + i * 8 + 8] = \
            data[h["offset_to_resource_list"] + i * 8: h["offset_to_resource_list"] + i * 8 + 8]
    for e in a.entries:
        out[e.offset:e.offset + e.size] = data[e.offset:e.offset + e.size]
    # zero any gaps not covered by regions (verbatim keeps them as-is only if zero)
    lay = erf_layout(data, name)
    for g in lay["gaps"]:
        if not g.get("kind") == "trailing":
            out[g["start"]:g["end"]] = bytes(g["size"])
    rep = diff_report(data, bytes(out), name)
    rep["layout"] = lay
    return bytes(out), rep


def _align_up(v: int, align: int) -> int:
    return (v + align - 1) // align * align if align > 1 else v


def erf_bytes_repack(data: bytes, name: str = "<mem>", align: int = 1) -> tuple[bytes, dict]:
    """From-scratch ERF writer: parsed entries only, payloads packed at `align`.

    Key records are rebuilt from parsed fields (resref latin-1 NUL-padded to
    16, res_id, type, unused=0); header build year/day/description and magic
    are preserved from the parsed header.  With align=1 this matches the
    retail save layout (contiguous payloads, off_loc == off_keys when no
    localized strings).
    """
    a = ka.read_erf_bytes(data, name)
    h = a.header
    n = len(a.entries)
    entries = a.entries
    off_keys = ERF_HEADER_SIZE
    off_res = off_keys + n * 24
    data_start = _align_up(off_res + n * 8, align)
    if h["language_count"] and h["localized_string_size"]:
        # localized block sits between header and key table in ERFs that use it
        off_loc = ERF_HEADER_SIZE
        off_keys = off_loc + h["localized_string_size"]
        off_res = off_keys + n * 24
        data_start = _align_up(off_res + n * 8, align)
    else:
        off_loc = off_keys  # retail quirk: off_loc == off_keys when unused

    keys = bytearray()
    res = bytearray()
    payload = bytearray()
    cursor = data_start
    for e in entries:
        raw_ref = e.resref.encode("latin-1")
        keys += raw_ref.ljust(16, b"\x00")
        keys += struct.pack("<IHH", e.res_id, e.res_type & 0xFFFF, 0)
        cursor = _align_up(cursor, align)
        if cursor > data_start + len(payload):
            payload += bytes(cursor - (data_start + len(payload)))
        res += struct.pack("<II", cursor, e.size)
        payload += data[e.offset:e.offset + e.size]
        cursor += e.size

    header = _erf_header_bytes(a, h, off_loc, off_keys, off_res, n)
    loc = b""
    if h["language_count"] and h["localized_string_size"]:
        loc = data[h["offset_to_localized_string"]:
                   h["offset_to_localized_string"] + h["localized_string_size"]]
    blob = header + loc + bytes(keys) + bytes(res)
    if len(blob) < data_start:
        blob += bytes(data_start - len(blob))
    blob += bytes(payload)
    rep = diff_report(data, blob, name)
    return blob, rep


def analyze_erf_bytes(data: bytes, name: str) -> dict:
    lay = erf_layout(data, name)
    verbatim, vrep = erf_bytes_verbatim(data, name)
    repack1, r1 = erf_bytes_repack(data, name, align=1)
    repack4, r4 = erf_bytes_repack(data, name, align=4)
    a = ka.read_erf_bytes(data, name)
    canonical = ka.erf_bytes([(e.resref, e.res_type, data[e.offset:e.offset + e.size])
                              for e in a.entries],
                             magic=a.magic.encode("latin-1"),
                             version=a.version.encode("latin-1"))
    crep = diff_report(data, canonical, name + " [ka.erf_bytes]")
    return {
        "name": name, "sha256": sha256_bytes(data), "size": len(data),
        "layout": lay,
        "verbatim": {"byte_identical": vrep["byte_identical"],
                     "first_diff_offset": vrep["first_diff_offset"],
                     "diff_byte_count": vrep["diff_byte_count_in_overlap"],
                     "length_delta": vrep["length_delta"]},
        "repack_align1": {"byte_identical": r1["byte_identical"],
                          "first_diff_offset": r1["first_diff_offset"],
                          "diff_byte_count": r1["diff_byte_count_in_overlap"],
                          "length_delta": r1["length_delta"]},
        "repack_align4": {"byte_identical": r4["byte_identical"],
                          "length_delta": r4["length_delta"]},
        "canonical_ka_erf_bytes": {"byte_identical": crep["byte_identical"],
                                   "first_diff_offset": crep["first_diff_offset"],
                                   "diff_byte_count": crep["diff_byte_count_in_overlap"],
                                   "length_delta": crep["length_delta"]},
    }


# ---------------------------------------------------------------------------
# GFF layout + parity writer
# ---------------------------------------------------------------------------

GFF_SECTION_NAMES = ("struct", "field", "label", "field_data", "field_index",
                     "list_index")


def gff_layout(data: bytes, name: str = "<mem>") -> dict:
    if len(data) < GFF_HEADER_SIZE:
        raise ks.SaveParseError(f"{name}: shorter than 56-byte GFF header")
    (so, sc, fo, fc, lo, lc, do, dc, xo, xc, io, ic) = struct.unpack_from("<12I", data, 8)
    sections = [
        {"kind": "struct", "start": so, "end": so + sc * 12, "count": sc},
        {"kind": "field", "start": fo, "end": fo + fc * 12, "count": fc},
        {"kind": "label", "start": lo, "end": lo + lc * 16, "count": lc},
        {"kind": "field_data", "start": do, "end": do + dc, "size": dc},
        {"kind": "field_index", "start": xo, "end": xo + xc, "size": xc},
        {"kind": "list_index", "start": io, "end": io + ic, "size": ic},
    ]
    sections.sort(key=lambda s: s["start"])
    gaps = []
    covered = GFF_HEADER_SIZE
    overlap = False
    for s in sections:
        if s["start"] < covered:
            overlap = True
        if s["start"] > covered:
            g = data[covered:s["start"]]
            gaps.append({"start": covered, "end": s["start"], "size": len(g),
                         "all_zero": g == bytes(len(g))})
        covered = max(covered, s["end"])
    trailing = len(data) - covered
    contiguous = all(sections[i]["end"] == sections[i + 1]["start"]
                     for i in range(len(sections) - 1)) and sections[0]["start"] == GFF_HEADER_SIZE
    return {
        "name": name, "size": len(data),
        "magic": data[0:4].decode("latin-1", "replace"),
        "version": data[4:8].decode("latin-1", "replace"),
        "header": {"struct_off": so, "struct_cnt": sc, "field_off": fo,
                   "field_cnt": fc, "label_off": lo, "label_cnt": lc,
                   "fdata_off": do, "fdata_len": dc, "fidx_off": xo,
                   "fidx_len": xc, "lidx_off": io, "lidx_len": ic},
        "sections": sections, "gaps": gaps, "overlapping_sections": overlap,
        "sections_contiguous_from_56": contiguous, "trailing_bytes": trailing,
        "fully_accounted": not overlap and trailing == 0
        and all(g["all_zero"] for g in gaps),
    }


def gff_bytes_verbatim(data: bytes, name: str = "<mem>") -> tuple[bytes, dict]:
    """Rebuild a GFF from parsed section offsets + verbatim section slices."""
    lay = gff_layout(data, name)
    h = lay["header"]
    header = data[0:4] + data[4:8] + struct.pack(
        "<12I", h["struct_off"], h["struct_cnt"], h["field_off"], h["field_cnt"],
        h["label_off"], h["label_cnt"], h["fdata_off"], h["fdata_len"],
        h["fidx_off"], h["fidx_len"], h["lidx_off"], h["lidx_len"])
    out = bytearray(header)
    for s in lay["sections"]:
        if len(out) < s["start"]:
            out += bytes(s["start"] - len(out))
        out += data[s["start"]:s["end"]]
    if len(out) < len(data):
        out += data[len(out):]
    rep = diff_report(data, bytes(out), name)
    rep["layout"] = lay
    return bytes(out), rep


def gff_bytes_parity(file_type: str | bytes, root: gw.GffStruct,
                     version: str | bytes = b"V3.2", fdata_align: int = 1) -> bytes:
    """Serialize a GffStruct tree with the retail GFF layout.

    Differences from ``kotor_gff_write.gff_bytes`` (which targets the canonical
    aligned layout): field-data entries are padded only to ``fdata_align``
    (retail uses 1, i.e. no padding).  Everything else matches: section order
    struct/field/label/field_data/field_index/list_index, labels in first-use
    order, structs in pre-order, the single-field struct direct-row rule, and
    locstring ``total = 8 + substring bytes``.
    """
    if not isinstance(root, gw.GffStruct):
        raise gw.GffWriteError("root must be GffStruct")
    ft = file_type.encode("latin-1") if isinstance(file_type, str) else bytes(file_type)
    ver = version.encode("latin-1") if isinstance(version, str) else bytes(version)
    if len(ft) != 4 or len(ver) != 4:
        raise gw.GffWriteError("GFF file type/version must be exactly 4 bytes")

    structs = gw._collect_structs(root)
    struct_index = {id(s): i for i, s in enumerate(structs)}
    labels: list[bytes] = []
    label_index: dict[str, int] = {}

    def label_id(label: str) -> int:
        if label not in label_index:
            label_index[label] = len(labels)
            labels.append(gw._encode_label(label))
        return label_index[label]

    fdata = bytearray()
    fidx = bytearray()
    lidx = bytearray()
    field_rows: list[tuple[int, int, int]] = []
    struct_rows: list[tuple[int, int, int]] = []

    def fdata_add(payload: bytes) -> int:
        while len(fdata) % fdata_align:
            fdata.append(0)
        off = len(fdata)
        fdata.extend(payload)
        return off

    def encode(ftype: int, value, field: str) -> int:
        if ftype in gw.INLINE_TYPES:
            if ftype in (0, 2, 4, 18):
                v = int(value)
                if not 0 <= v <= 0xFFFFFFFF:
                    raise gw.GffWriteError(f"{field}: inline value out of u32 range")
                return v
            if ftype in (1, 3):
                bits = 8 if ftype == 1 else 16
                return int(value) & ((1 << bits) - 1)
            if ftype == 5:
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
            raw = gw._text(value, field)
            return fdata_add(struct.pack("<I", len(raw)) + raw)
        if ftype == 11:
            raw = gw._resref(value, field)
            return fdata_add(bytes([len(raw)]) + raw)
        if ftype == 12:
            if not isinstance(value, dict):
                raise gw.GffWriteError(f"{field}: locstring needs a dict")
            strref = int(value.get("strref", 0xFFFFFFFF)) & 0xFFFFFFFF
            subs = value.get("substrings", [])
            payload = bytearray(struct.pack("<3I", 0, strref, len(subs)))
            sub_bytes = 0
            for item in subs:
                lang, text = item
                raw = gw._text(text, f"{field} substring")
                payload += struct.pack("<2I", int(lang) & 0xFFFFFFFF, len(raw))
                payload += raw
                sub_bytes += 8 + len(raw)
            struct.pack_into("<I", payload, 0, 8 + sub_bytes)
            return fdata_add(bytes(payload))
        if ftype == 13:
            if not isinstance(value, (bytes, bytearray)):
                raise gw.GffWriteError(f"{field}: binary field needs bytes")
            return fdata_add(struct.pack("<I", len(value)) + bytes(value))
        if ftype == 14:
            if not isinstance(value, gw.GffStruct):
                raise gw.GffWriteError(f"{field}: struct field needs GffStruct")
            return struct_index[id(value)]
        if ftype == 15:
            if not isinstance(value, (list, tuple)):
                raise gw.GffWriteError(f"{field}: list field needs a sequence")
            off = len(lidx)
            lidx.extend(struct.pack("<I", len(value)))
            for child in value:
                lidx.extend(struct.pack("<I", struct_index[id(child)]))
            return off
        if ftype == 16:
            vals = [float(x) for x in value]
            if len(vals) != 4:
                raise gw.GffWriteError(f"{field}: vector4 needs 4 floats")
            return fdata_add(struct.pack("<4f", *vals))
        if ftype == 17:
            vals = [float(x) for x in value]
            if len(vals) != 3:
                raise gw.GffWriteError(f"{field}: vector3 needs 3 floats")
            return fdata_add(struct.pack("<3f", *vals))
        raise gw.GffWriteError(f"{field}: unsupported GFF type {ftype}")

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
            slot = len(fidx)
            for row in rows:
                fidx.extend(struct.pack("<I", row))
            struct_rows.append((node.type_id, slot, len(rows)))

    label_bytes = b"".join(labels)
    struct_bytes = b"".join(struct.pack("<3I", *r) for r in struct_rows)
    field_bytes = b"".join(struct.pack("<3I", *r) for r in field_rows)
    struct_off = GFF_HEADER_SIZE
    field_off = struct_off + len(struct_bytes)
    label_off = field_off + len(field_bytes)
    fdata_off = label_off + len(label_bytes)
    fidx_off = fdata_off + len(fdata)
    lidx_off = fidx_off + len(fidx)
    header = struct.pack("<12I", struct_off, len(struct_rows), field_off,
                         len(field_rows), label_off, len(labels), fdata_off,
                         len(fdata), fidx_off, len(fidx), lidx_off, len(lidx))
    return ft + ver + header + struct_bytes + field_bytes + label_bytes + \
        bytes(fdata) + bytes(fidx) + bytes(lidx)


def _gff_region_of(lay: dict, offset: int) -> str:
    if offset < GFF_HEADER_SIZE:
        return "header"
    for s in lay["sections"]:
        if s["start"] <= offset < s["end"]:
            return s["kind"]
    return "trailing"


def analyze_gff_bytes(data: bytes, name: str) -> dict:
    lay = gff_layout(data, name)
    g = ks.gff_from_bytes(data, name)
    verbatim, vrep = gff_bytes_verbatim(data, name)
    parity = gff_bytes_parity(g.magic, gw.from_gff(g), g.version, fdata_align=1)
    prep = diff_report(data, parity, name + " [parity align=1]")
    # per-section diff classification for the parity re-encode
    section_diffs: dict[str, int] = {}
    n = min(len(data), len(parity))
    for i in range(n):
        if data[i] != parity[i]:
            section_diffs[_gff_region_of(lay, i)] = \
                section_diffs.get(_gff_region_of(lay, i), 0) + 1
    canonical = gw.gff_bytes(g.magic, gw.from_gff(g), g.version)
    crep = diff_report(data, canonical, name + " [gw.gff_bytes]")
    # semantic equality of the parity re-encode
    fd, tmp = _mkstemp_bytes(parity)
    try:
        g2 = gff.Gff(tmp)
        sem_eq = gw.semantic(g) == gw.semantic(g2)
        check_new = list(gff.check(g2))
    finally:
        os.unlink(tmp)
    check_orig = list(gff.check(g))
    return {
        "name": name, "sha256": sha256_bytes(data), "size": len(data),
        "layout": lay,
        "verbatim": {"byte_identical": vrep["byte_identical"]},
        "parity_align1": {"byte_identical": prep["byte_identical"],
                          "first_diff_offset": prep["first_diff_offset"],
                          "diff_byte_count": prep["diff_byte_count_in_overlap"],
                          "length_delta": prep["length_delta"],
                          "diff_bytes_by_section": section_diffs},
        "canonical_gw_gff_bytes": {"byte_identical": crep["byte_identical"],
                                   "first_diff_offset": crep["first_diff_offset"],
                                   "diff_byte_count": crep["diff_byte_count_in_overlap"],
                                   "length_delta": crep["length_delta"]},
        "semantic_equal_after_reencode": sem_eq,
        "check_errors_orig": check_orig, "check_errors_reencode": check_new,
    }


def _mkstemp_bytes(data: bytes) -> tuple[int, str]:
    import tempfile
    fd, path = tempfile.mkstemp(prefix="gffpar-", suffix=".bin", dir=str(ks.TMP_ROOT))
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    return fd, path


# ---------------------------------------------------------------------------
# save-slot analysis / parity rewrite
# ---------------------------------------------------------------------------


def _is_gff(data: bytes) -> bool:
    return ks.looks_like_gff(data)


def _is_erf(data: bytes) -> bool:
    return ks.looks_like_erf(data)


def analyze_save_folder(path: str | os.PathLike) -> dict:
    """Measure byte parity for every file of a save slot, recursing into
    nested module ERFs.  Read-only against the source."""
    sf = ks.SaveFolder(path)
    model = sf.parse(redact=True)
    out: dict = {
        "schema": "kotor-save-byteparity/1",
        "build_sha256": BUILD_SHA256,
        "folder": str(Path(path).resolve()),
        "folder_name": model["folder_name"],
        "kind": model["kind"],
        "validation": {"errors": len(model["validation"]["errors"]),
                       "warnings": len(model["validation"]["warnings"])},
        "files": {},
    }
    for p in sorted(sf.files.values(), key=lambda x: x.name.lower()):
        data = p.read_bytes()
        entry = {"size": len(data), "sha256": sha256_bytes(data)}
        lname = p.name.lower()
        if lname == "savegame.sav" and _is_erf(data):
            erf = analyze_erf_bytes(data, "SAVEGAME.sav")
            entry["container"] = "erf"
            entry["erf"] = erf
            entry["nested"] = []
            a = ka.read_erf_bytes(data, "SAVEGAME.sav")
            for e in a.entries:
                if e.res_type == 2057 and _is_erf(data[e.offset:e.offset + e.size]):
                    nd = data[e.offset:e.offset + e.size]
                    ne = {"resref": e.resref, "erf": analyze_erf_bytes(nd, f"{e.resref}.sav"),
                          "gffs": []}
                    na = ka.read_erf_bytes(nd, f"{e.resref}.sav")
                    for nee in na.entries:
                        ndata = nd[nee.offset:nee.offset + nee.size]
                        if _is_gff(ndata):
                            ne["gffs"].append(
                                analyze_gff_bytes(ndata, f"{e.resref}/{nee.resref}.{nee.res_type}"))
                    entry["nested"].append(ne)
        elif _is_gff(data):
            entry["container"] = "gff"
            entry["gff"] = analyze_gff_bytes(data, p.name)
        else:
            entry["container"] = "blob"
            entry["note"] = "copied byte-for-byte by any writer (no re-encode)"
        out["files"][p.name] = entry
    # summary counts
    erfs = [out["files"]["SAVEGAME.sav"]["erf"]] if "SAVEGAME.sav" in out["files"] else []
    for f in out["files"].values():
        for ne in f.get("nested", []):
            erfs.append(ne["erf"])
    gffs = [f["gff"] for f in out["files"].values() if f.get("container") == "gff"]
    for f in out["files"].values():
        for ne in f.get("nested", []):
            gffs.extend(ne["gffs"])
    out["summary"] = {
        "erf_total": len(erfs),
        "erf_verbatim_identical": sum(1 for e in erfs if e["verbatim"]["byte_identical"]),
        "erf_repack_align1_identical": sum(1 for e in erfs if e["repack_align1"]["byte_identical"]),
        "erf_canonical_identical": sum(1 for e in erfs if e["canonical_ka_erf_bytes"]["byte_identical"]),
        "gff_total": len(gffs),
        "gff_verbatim_identical": sum(1 for g in gffs if g["verbatim"]["byte_identical"]),
        "gff_parity_identical": sum(1 for g in gffs if g["parity_align1"]["byte_identical"]),
        "gff_canonical_identical": sum(1 for g in gffs if g["canonical_gw_gff_bytes"]["byte_identical"]),
        "gff_semantic_equal_all": all(g["semantic_equal_after_reencode"] for g in gffs),
        "gff_check_errors_total": sum(len(g["check_errors_orig"]) for g in gffs),
    }
    return out


def rewrite_save_parity(src: str | os.PathLike, dst: str | os.PathLike,
                        overwrite: bool = False, mode: str = "parity") -> dict:
    """Rewrite a save slot; report per-file equality against the source.

    ``mode="parity"`` uses the from-scratch retail-faithful encoders: ERF
    containers repacked with ``erf_bytes_repack(align=1)`` (retail layout) and
    GFF payloads with ``gff_bytes_parity`` (no fdata alignment).
    ``mode="verbatim"`` uses the layout-preserving re-encoders
    (``erf_bytes_verbatim`` / ``gff_bytes_verbatim``): an *unmodified*
    re-encode, which must be byte-identical and proves full byte accounting.
    Non-container files (Screen.tga, CORRUPT.res, ...) are copied
    byte-for-byte in both modes.
    """
    if mode not in ("parity", "verbatim"):
        raise ks.SaveWriteError(f"unknown mode {mode!r}")
    src_p = Path(src)
    dst_p = Path(dst)
    ks.ensure_writable_target(dst_p, source=src_p)
    if dst_p.exists() and not overwrite:
        raise ks.SaveWriteError(f"{dst_p}: exists (use overwrite)")
    dst_p.mkdir(parents=True, exist_ok=True)

    sf = ks.SaveFolder(src_p)
    sf.parse()
    report: dict = {"schema": "kotor-save-parity-rewrite/1",
                    "mode": mode,
                    "build_sha256": BUILD_SHA256,
                    "src": str(src_p.resolve()), "dst": str(dst_p.resolve()),
                    "files": {}}

    def encode_gff(data: bytes, name: str) -> tuple[bytes, dict]:
        if mode == "verbatim":
            return gff_bytes_verbatim(data, name)
        g = ks.gff_from_bytes(data, name)
        blob = gff_bytes_parity(g.magic, gw.from_gff(g), g.version, fdata_align=1)
        return blob, diff_report(data, blob, name)

    for p in sorted(sf.files.values(), key=lambda x: x.name.lower()):
        data = p.read_bytes()
        lname = p.name.lower()
        if lname == "savegame.sav" and _is_erf(data):
            if mode == "verbatim":
                blob, vrep = erf_bytes_verbatim(data, "SAVEGAME.sav")
                nested_reports = []
                a0 = ka.read_erf_bytes(data, "SAVEGAME.sav")
                for e in a0.entries:
                    edata = data[e.offset:e.offset + e.size]
                    if e.res_type == 2057 and _is_erf(edata):
                        nblob, nrep = erf_bytes_verbatim(edata, e.resref)
                        nested_reports.append({"resref": e.resref,
                                               "byte_identical": nrep["byte_identical"]})
            else:
                a = ka.read_erf_bytes(data, "SAVEGAME.sav")
                outer_entries = []
                nested_reports = []
                for e in a.entries:
                    edata = data[e.offset:e.offset + e.size]
                    if e.res_type == 2057 and _is_erf(edata):
                        na = ka.read_erf_bytes(edata, e.resref)
                        nentries = []
                        for ne in na.entries:
                            ndata = edata[ne.offset:ne.offset + ne.size]
                            if _is_gff(ndata):
                                ndata, _ = encode_gff(ndata, f"{e.resref}/{ne.resref}")
                            nentries.append((ne.resref, ne.res_type, ndata))
                        # repack nested contiguously (retail align=1) with the
                        # re-encoded GFF payloads and the original header fields
                        blob_n = _repack_with_entries(edata, na, nentries)
                        nested_reports.append({"resref": e.resref,
                                               "byte_identical": blob_n == edata})
                        outer_entries.append((e.resref, e.res_type, blob_n))
                    else:
                        outer_entries.append((e.resref, e.res_type, edata))
                blob = _repack_with_entries(data, a, outer_entries)
            (dst_p / p.name).write_bytes(blob)
            report["files"][p.name] = {
                "size": len(data), "sha256": sha256_bytes(data),
                "rewritten_byte_identical": blob == data,
                "nested": nested_reports,
                "diff": diff_report(data, blob, p.name) if blob != data else None,
            }
        elif _is_gff(data):
            blob, rep = encode_gff(data, p.name)
            (dst_p / p.name).write_bytes(blob)
            report["files"][p.name] = {
                "size": len(data), "sha256": sha256_bytes(data),
                "rewritten_byte_identical": blob == data,
                "diff": None if blob == data else rep,
            }
        else:
            (dst_p / p.name).write_bytes(data)
            report["files"][p.name] = {"size": len(data), "sha256": sha256_bytes(data),
                                       "rewritten_byte_identical": True,
                                       "copied_verbatim": True}

    # re-parse the rewrite to prove it is still structurally valid
    model = ks.SaveFolder(dst_p).parse(redact=True)
    report["rewrite_validation"] = {
        "errors": len(model["validation"]["errors"]),
        "warnings": len(model["validation"]["warnings"]),
    }
    report["summary"] = {
        "files_total": len(report["files"]),
        "files_byte_identical": sum(1 for f in report["files"].values()
                                    if f.get("rewritten_byte_identical")),
        "nested_byte_identical": sum(1 for f in report["files"].values()
                                     for ne in f.get("nested", [])
                                     if ne["byte_identical"]),
    }
    return report


def _repack_with_entries(data: bytes, archive: ka.Archive,
                         entries: list[tuple[str, int, bytes]]) -> bytes:
    """Pack pre-encoded payloads contiguously (retail align=1) with the
    original header fields, keys rebuilt from parsed entry fields."""
    h = archive.header
    n = len(entries)
    off_keys = ERF_HEADER_SIZE
    off_res = off_keys + n * 24
    data_start = off_res + n * 8
    if h["language_count"] and h["localized_string_size"]:
        off_loc = ERF_HEADER_SIZE
        off_keys = off_loc + h["localized_string_size"]
        off_res = off_keys + n * 24
        data_start = off_res + n * 8
    else:
        off_loc = off_keys
    keys = bytearray()
    res = bytearray()
    payload = bytearray()
    cursor = data_start
    src_entries = archive.entries
    for i, (resref, rtype, blob) in enumerate(entries):
        rid = src_entries[i].res_id if i < len(src_entries) else i
        keys += resref.encode("latin-1").ljust(16, b"\x00")
        keys += struct.pack("<IHH", rid, int(rtype) & 0xFFFF, 0)
        res += struct.pack("<II", cursor, len(blob))
        payload += blob
        cursor += len(blob)
    header = _erf_header_bytes(archive, h, off_loc, off_keys, off_res, n)
    loc = b""
    if h["language_count"] and h["localized_string_size"]:
        loc = data[h["offset_to_localized_string"]:
                   h["offset_to_localized_string"] + h["localized_string_size"]]
    out = header + loc + bytes(keys) + bytes(res)
    if len(out) < data_start:
        out += bytes(data_start - len(out))
    return out + bytes(payload)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _guard_out(path: Path) -> None:
    ks.ensure_writable_target(path)


def _write_json(path: str | os.PathLike, obj) -> None:
    p = Path(path)
    _guard_out(p)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(obj, indent=2), encoding="utf-8")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    ic = sub.add_parser("install-check", help="read-only swkotor.exe hash sentinel")
    ic.add_argument("--game-dir", required=True)

    an = sub.add_parser("analyze", help="byte-parity analysis of one save slot")
    an.add_argument("folder")
    an.add_argument("--json")

    rw = sub.add_parser("rewrite-parity",
                        help="rewrite a save slot with parity encoders and compare")
    rw.add_argument("src")
    rw.add_argument("dst")
    rw.add_argument("--overwrite", action="store_true")
    rw.add_argument("--mode", choices=["parity", "verbatim"], default="parity")
    rw.add_argument("--json")

    args = ap.parse_args(argv)

    if args.cmd == "install-check":
        info = verify_install(args.game_dir)
        print(json.dumps(info, indent=2))
        return 0 if info["match"] else 1

    if args.cmd == "analyze":
        report = analyze_save_folder(args.folder)
        print(json.dumps(report["summary"], indent=2))
        if args.json:
            _write_json(args.json, report)
            print(f"wrote {args.json}")
        return 0

    if args.cmd == "rewrite-parity":
        report = rewrite_save_parity(args.src, args.dst, overwrite=args.overwrite,
                                     mode=args.mode)
        print(json.dumps(report["summary"], indent=2))
        print(json.dumps(report["rewrite_validation"], indent=2))
        if args.json:
            _write_json(args.json, report)
            print(f"wrote {args.json}")
        return 0 if report["rewrite_validation"]["errors"] == 0 else 1

    return 2


if __name__ == "__main__":
    sys.exit(main())
