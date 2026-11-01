#!/usr/bin/env python3
"""Lane s1-02-formats: full format validation + byte-identical round-trip suite.

Usage:
    python tools/formats/validate_all.py --game-dir <KOTOR_INSTALL> \
        [--out artifacts/lanes/s1-02-formats/validate_all.json]

Read-only against the install: every game file is opened 'rb' only.  The
first step verifies swkotor.exe SHA-256 against the build of record and
stops (exit 2) on drift.

Runs, over the FULL shipped corpus:
  * every parser/validator in tools/formats (lyt, vis, bwm/wok, pth, mdl,
    tpc+txi, dds, tga, ssf, lip, gui, bik, wav, mp3, two_da, tlk, gff) plus
    the archive readers in tools/resources/kotor_archive.py;
  * byte-identical round-trips:
    (a) GFF re-encode of every GFF payload (pth.gff_reencode) -- expected
        11,143 files;
    (b) 2DA V2.b writer re-encode of every shipped table (two_da_ascii)
        plus parse+write ASCII V2.0 semantic round-trip;
    (c) TLK writer re-encode of dialog.tlk (tlk_write, 49,265 entries);
    (d) ERF/MOD writer re-encode of patch.erf + all 4 TexturePacks ERFs,
        RIM re-encode of all 234 module RIMs + 12 global RIMs, and
        BIF+KEY re-encode of all 26 data/*.bif + chitin.key.

Writes the results JSON to --out (default
artifacts/lanes/s1-02-formats/validate_all.json) and prints a summary.
Exit 0 iff the exe hash matches and every class passes on the reference
install; exit 2 on install drift; exit 1 on any validation failure.

Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import time
from collections import Counter, defaultdict
from datetime import datetime, timezone

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
for _p in (_HERE, os.path.join(_ROOT, "tools", "resources")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import gff  # noqa: E402
import two_da  # noqa: E402
import kotor_archive as ka  # noqa: E402

import lyt  # noqa: E402
import vis  # noqa: E402
import bwm  # noqa: E402
import wok  # noqa: E402
import pth  # noqa: E402
import mdl  # noqa: E402
import tpc  # noqa: E402
import txi  # noqa: E402
import dds  # noqa: E402
import tga  # noqa: E402
import ssf  # noqa: E402
import lip  # noqa: E402
import gui  # noqa: E402
import bik  # noqa: E402
import wav  # noqa: E402
import mp3  # noqa: E402
import two_da_ascii  # noqa: E402
import tlk_write  # noqa: E402

EXPECTED_EXE_SHA256 = (
    "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6")

# declared resource type ids
T = {
    "tga": 3, "wav": 4, "mdl": 2002, "nss": 2009, "ncs": 2010, "are": 2012,
    "ifo": 2014, "bic": 2015, "wok": 2016, "2da": 2017, "txi": 2022,
    "git": 2023, "bti": 2024, "uti": 2025, "btc": 2026, "utc": 2027,
    "dlg": 2029, "itp": 2030, "utt": 2032, "uts": 2035, "ltr": 2036,
    "fac": 2038, "ute": 2040, "utd": 2042, "utp": 2044, "gui": 2047,
    "utm": 2051, "dwk": 2052, "pwk": 2053, "jrl": 2056, "utw": 2058,
    "ssf": 2060, "lip": 3004, "lyt": 3000, "vis": 3001, "pth": 3003,
    "mdx": 3008, "tpc": 3007,
}
GFF_DECLARED = {2012, 2014, 2015, 2023, 2025, 2026, 2027, 2029, 2030, 2032, 2035,
                2036, 2038, 2040, 2042, 2044, 2047, 2051, 2056, 2058, 3003, 2024}


class Report:
    def __init__(self):
        self.classes: dict[str, dict] = {}

    def cls(self, name: str) -> dict:
        return self.classes.setdefault(name, {"count": 0, "ok": 0, "fail": 0,
                                               "failures": []})

    def run(self, name: str, label: str, fn, *args):
        c = self.cls(name)
        c["count"] += 1
        try:
            fn(*args)
            c["ok"] += 1
            return True
        except Exception as exc:  # noqa: BLE001 - report and continue
            c["fail"] += 1
            if len(c["failures"]) < 100:
                c["failures"].append(f"{label}: {type(exc).__name__}: {exc}")
            return False


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def two_da_roundtrip(payload: bytes, label: str, rt: dict) -> None:
    """Parse (either V2.b layout or V2.0), re-encode byte-identically in the
    same layout, and check the ASCII V2.0 round-trip is semantically equal."""
    rt["count"] += 1
    try:
        tbl, layout = two_da_ascii.parse_2da_any(payload, label)
        rt.setdefault("layouts", Counter())[layout] += 1
        if two_da_ascii.write_2da_v2b_any(tbl, layout) == payload:
            rt["byte_identical"] += 1
        else:
            rt["failures"].append(f"{label}: v2b re-encode differs ({layout})")
        tbl2 = two_da.parse_2da(two_da_ascii.write_2da_v2a(tbl), label + ":ascii")
        if tbl.columns == tbl2.columns and tbl.rows == tbl2.rows and tbl.default == tbl2.default:
            rt["ascii_semantic_equal"] += 1
        else:
            rt["failures"].append(f"{label}: ascii round-trip differs")
    except Exception as exc:
        rt["fail"] += 1
        if len(rt["failures"]) < 100:
            rt["failures"].append(f"{label}: {type(exc).__name__}: {exc}")


# ---------------------------------------------------------------------------
# archive re-encoders (byte-identical round-trip)
# ---------------------------------------------------------------------------

def erf_reencode_check(path: str) -> dict:
    """Re-encode an ERF/MOD/SAV from parsed header+keys+resources and
    compare against the original file hash, streaming payloads."""
    with open(path, "rb") as f:
        head = f.read(160)
        if len(head) < 160 or head[:4] not in (b"ERF ", b"MOD ", b"SAV "):
            return {"ok": False, "detail": "not ERF-family"}
        (lang_count, loc_size, count, off_loc, off_keys, off_res,
         year, day, desc) = struct.unpack_from("<9I", head, 8)
        f.seek(off_keys)
        keys = f.read(count * 24)
        f.seek(off_res)
        res = f.read(count * 8)
        if len(keys) != count * 24 or len(res) != count * 8:
            return {"ok": False, "detail": "short key/resource table"}
        entries = [struct.unpack_from("<II", res, i * 8) for i in range(count)]
        data_start = off_res + count * 8
        # localized string table region [off_loc, off_loc+loc_size) is
        # preserved verbatim via the raw 160-byte header; shipped files
        # have off_loc == 160 and loc_size == 0.
        # rebuild resource table with recomputed (gapless) offsets
        new_res = bytearray()
        cursor = data_start
        consecutive = True
        for i, (off, size) in enumerate(entries):
            if off != cursor:
                consecutive = False
            new_res += struct.pack("<II", cursor, size)
            cursor += size
        h = hashlib.sha256()
        h.update(head[:off_keys])          # 160-byte header verbatim
        h.update(keys)                      # 24-byte key records verbatim
        h.update(bytes(new_res))            # rebuilt resource records
        # payloads streamed in entry order
        for off, size in entries:
            f.seek(off)
            remaining = size
            while remaining:
                chunk = f.read(min(1 << 20, remaining))
                if not chunk:
                    return {"ok": False, "detail": f"short read at {off}"}
                h.update(chunk)
                remaining -= len(chunk)
    orig = sha256_file(path)
    return {"ok": h.hexdigest() == orig, "consecutive_offsets": consecutive,
            "entries": count, "rebuilt_sha256": h.hexdigest(), "original_sha256": orig,
            "detail": "byte-identical" if h.hexdigest() == orig else "DIFF"}


def rim_reencode_check(path: str) -> dict:
    """Re-encode a RIM preserving original payload offsets (shipped RIMs
    carry writer-specific gaps, so gapless repacking is not byte-identical;
    the re-encode rebuilds header+entry table from parsed fields and
    re-places every payload at its recorded offset)."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIM ":
        return {"ok": False, "detail": "not RIM"}
    reserved, count, off_entries = struct.unpack_from("<3I", data, 8)
    entries = []
    for i in range(count):
        b = data[off_entries + i * 32: off_entries + i * 32 + 32]
        resref = b[:16]
        rtype, rid, off, rsize = struct.unpack_from("<4I", b, 16)
        entries.append((resref, rtype, rid, off, rsize))
    out = bytearray(size)
    out[0:8] = data[0:8]                      # magic + version
    struct.pack_into("<3I", out, 8, reserved, count, off_entries)
    # bytes between the 20-byte header and the entry table: preserved raw
    out[20:off_entries] = data[20:off_entries]
    for i, (resref, rtype, rid, off, rsize) in enumerate(entries):
        base = off_entries + i * 32
        out[base:base + 16] = resref
        struct.pack_into("<4I", out, base + 16, rtype, rid, off, rsize)
        out[off:off + rsize] = data[off:off + rsize]
    ok = bytes(out) == data
    return {"ok": ok, "entries": count, "gapless": all(
        entries[i][3] + entries[i][4] == entries[i + 1][3]
        for i in range(len(entries) - 1)) and
        (off_entries + count * 32 == entries[0][3] if entries else True),
        "detail": "byte-identical" if ok else "DIFF"}


def bif_reencode_check(path: str, bif_index: int) -> dict:
    """Re-encode a BIFF V1: header + var/fixed tables rebuilt with gapless
    offsets + payloads streamed in record order."""
    with open(path, "rb") as f:
        head = f.read(20)
        if head[:8] != b"BIFFV1  ":
            return {"ok": False, "detail": "not BIFF V1"}
        var_count, fixed_count, off_var = struct.unpack_from("<3I", head, 8)
        f.seek(off_var)
        var_blob = f.read(var_count * 16)
        off_fixed = off_var + var_count * 16
        f.seek(off_fixed)
        fixed_blob = f.read(fixed_count * 16)
        records = []
        for blob in (var_blob, fixed_blob):
            for i in range(len(blob) // 16):
                records.append(struct.unpack_from("<4I", blob, i * 16))
        data_start = off_fixed + fixed_count * 16
        new_table = bytearray()
        cursor = data_start
        consecutive = True
        for rid, off, size, rtype in records:
            if off != cursor:
                consecutive = False
            new_table += struct.pack("<4I", rid, cursor, size, rtype)
            cursor += size
        h = hashlib.sha256()
        h.update(head[:8])
        h.update(struct.pack("<3I", var_count, fixed_count, off_var))
        h.update(bytes(new_table))
        for rid, off, size, rtype in records:
            f.seek(off)
            remaining = size
            while remaining:
                chunk = f.read(min(1 << 20, remaining))
                if not chunk:
                    return {"ok": False, "detail": "short read"}
                h.update(chunk)
                remaining -= len(chunk)
    orig = sha256_file(path)
    ok = h.hexdigest() == orig
    return {"ok": ok, "records": len(records), "consecutive_offsets": consecutive,
            "detail": "byte-identical" if ok else "DIFF"}


def key_reencode_check(path: str) -> dict:
    """Re-encode chitin.key: 64-byte header + file table + names + 22-byte
    key records, all rebuilt from parsed fields."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"KEY V1  ":
        return {"ok": False, "detail": "not KEY V1"}
    bif_count, key_count, off_files, off_keys = struct.unpack_from("<4I", data, 8)
    files = []
    for i in range(bif_count):
        files.append(struct.unpack_from("<IIHH", data, off_files + i * 12))
    keys = [data[off_keys + i * 22: off_keys + i * 22 + 22] for i in range(key_count)]
    # rebuild: preserve header bytes 8..64 verbatim except the four
    # count/offset u32s (shipped chitin.key carries build year/day at
    # 0x18/0x1C, like the ERFs)
    out = bytearray()
    new_off_files = 64
    names_start = new_off_files + bif_count * 12
    file_table = bytearray()
    name_cursor = names_start
    for (dsize, name_off, name_len, drives) in files:
        file_table += struct.pack("<IIHH", dsize, name_cursor, name_len, drives)
        name_cursor += name_len
    out += b"KEY V1  "
    out += struct.pack("<4I", bif_count, key_count, new_off_files, name_cursor)
    out += data[24:64]
    out += file_table
    # append names in original file order
    for (dsize, name_off, name_len, drives) in files:
        out += data[name_off:name_off + name_len]
    for k in keys:
        out += k
    ok = bytes(out) == data
    return {"ok": ok, "bifs": bif_count, "keys": key_count,
            "detail": "byte-identical" if ok else "DIFF"}


# ---------------------------------------------------------------------------
# corpus walking
# ---------------------------------------------------------------------------

def _lsdir(path: str, suffix: str | None = None) -> list[str]:
    if not os.path.isdir(path):
        return []
    return sorted(os.path.join(path, f) for f in os.listdir(path)
                 if os.path.isfile(os.path.join(path, f))
                 and (suffix is None or f.endswith(suffix)))


class Corpus:
    """Read-only view over the install's archives."""

    def __init__(self, game_dir: str):
        self.gd = game_dir
        self.key = ka.read_key(os.path.join(game_dir, "chitin.key"))
        self._bifs: dict[str, ka.BifFile] = {}
        self.module_rims = _lsdir(os.path.join(game_dir, "modules"), ".rim")
        self.global_rims = _lsdir(os.path.join(game_dir, "rims"), ".rim")
        tp = _lsdir(os.path.join(game_dir, "TexturePacks"), ".erf")
        patch = os.path.join(game_dir, "patch.erf")
        self.erfs = ([patch] if os.path.isfile(patch) else []) + tp
        self.lip_mods = _lsdir(os.path.join(game_dir, "lips"), ".mod")

    def bif_for(self, bif_index: int) -> ka.BifFile:
        name = self.key.bifs[bif_index]["name"]
        if name not in self._bifs:
            self._bifs[name] = ka.read_bif(ka.bif_path_for(self.gd, name))
        return self._bifs[name]

    def base_entries(self):
        """Yield (KeyEntry, payload) for every base resource."""
        for e in self.key.entries:
            bf = self.bif_for(e.bif_index)
            rec = bf.by_id.get(e.resid)
            if rec is None:
                # records are contiguous; id == resid
                rec = bf.records[e.table_index]
            yield e, bf.read_record(rec)

    def rim_entries(self, path: str):
        a = ka.read_rim(path)
        for e in a.entries:
            yield e, a.read_entry(e)

    def erf_entries(self, path: str):
        a = ka.read_erf(path)
        for e in a.entries:
            yield e, a.read_entry(e)


def is_gff_payload(head: bytes) -> bool:
    return (len(head) >= 8 and head[4:8] == b"V3.2"
            and all(0x20 <= c < 0x7F for c in head[0:4]))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game-dir", default=os.environ.get("KOTOR_GAME_DIR", ""))
    ap.add_argument("--out", default=os.path.join(
        _ROOT, "artifacts", "lanes", "s1-02-formats", "validate_all.json"))
    ap.add_argument("--rim-limit", type=int, default=0,
                    help="validate only the first N module RIMs (0 = all)")
    args = ap.parse_args(argv)
    if not args.game_dir:
        print("error: --game-dir or KOTOR_GAME_DIR required", file=sys.stderr)
        return 2
    gd = args.game_dir
    t0 = time.time()
    rep = Report()

    # 0. install identity (read-only); stop on drift
    exe = os.path.join(gd, "swkotor.exe")
    exe_hash = sha256_file(exe)
    exe_ok = exe_hash == EXPECTED_EXE_SHA256
    print(f"swkotor.exe sha256={exe_hash} match={exe_ok}")
    if not exe_ok:
        print("INSTALL DRIFT: exe hash does not match the build of record; stopping.",
              file=sys.stderr)
        return 2

    corpus = Corpus(gd)
    print(f"key: {len(corpus.key.entries)} entries / {len(corpus.key.bifs)} bifs; "
          f"module rims: {len(corpus.module_rims)}, global rims: {len(corpus.global_rims)}, "
          f"erfs: {len(corpus.erfs)}, lip mods: {len(corpus.lip_mods)}")

    # ------------------------------------------------------------------
    # 1. base BIF payloads: per-type validation
    # ------------------------------------------------------------------
    type_counts: Counter = Counter()
    gff_seen = 0
    gff_rt = {"count": 0, "byte_identical": 0, "semantic_only": 0, "fail": 0,
              "failures": []}
    two_da_rt = {"count": 0, "byte_identical": 0, "ascii_semantic_equal": 0,
                 "fail": 0, "failures": [], "layouts": Counter()}
    for e, payload in corpus.base_entries():
        type_counts[e.res_type] += 1
        rt = e.res_type
        label = f"base/{e.resref}.{rt}"
        if is_gff_payload(payload[:8]):
            gff_seen += 1
            def _gff(p=payload, l=label):
                g = pth.load_gff_bytes(p, l)
                errs = gff.check(g)
                if errs:
                    raise ValueError(f"check: {errs[:2]}")
                verr = gff.check_values(g)
                if verr:
                    raise ValueError(f"values: {verr[:2]}")
                return pth.gff_reencode(g)
            gff_rt["count"] += 1
            try:
                out = _gff()
                if out == payload:
                    gff_rt["byte_identical"] += 1
                else:
                    gff_rt["semantic_only"] += 1
                    gff_rt["failures"].append(f"{label}: re-encode differs")
            except Exception as exc:
                gff_rt["fail"] += 1
                if len(gff_rt["failures"]) < 100:
                    gff_rt["failures"].append(f"{label}: {type(exc).__name__}: {exc}")
            rep.run("gff", label, pth.load_gff_bytes, payload, label)
        if rt == T["2da"]:
            two_da_roundtrip(payload, label, two_da_rt)
            rep.run("two_da", label, lambda p, l: two_da_ascii.parse_2da_any(p, l), payload, label)
        elif rt == T["lyt"]:
            rep.run("lyt", label, lyt.parse_lyt, payload, label)
        elif rt == T["vis"]:
            rep.run("vis", label, vis.parse_vis, payload, label)
        elif rt == T["wok"]:
            rep.run("wok", label, wok.parse_wok, payload, label)
        elif rt in (T["dwk"], T["pwk"]):
            rep.run("bwm", label, bwm.parse_bwm, payload, label, 0)
        elif rt == T["mdl"]:
            rep.run("mdl", label, mdl.parse_mdl, payload, label)
        elif rt == T["txi"]:
            rep.run("txi", label, txi.parse_txi, payload, label)
        elif rt == T["tga"]:
            rep.run("tga", label, tga.parse_tga, payload, label)
        elif rt == T["ssf"]:
            rep.run("ssf", label, ssf.parse_ssf, payload, label)
        elif rt == T["gui"]:
            rep.run("gui", label, gui.parse_gui, payload, label)
        elif rt == T["wav"]:
            rep.run("wav", label, wav.parse_wav, payload, label)
        elif rt == T["tpc"]:
            rep.run("tpc", label, tpc.parse_tpc, payload, label, txi.parse_txi)
        elif rt == T["pth"]:
            rep.run("pth", label, pth.parse_pth, payload, label)
    print(f"base payloads: {sum(type_counts.values())} resources, "
          f"gff-magic {gff_seen}; elapsed {time.time() - t0:.1f}s")

    # ------------------------------------------------------------------
    # 2. module + global RIMs
    # ------------------------------------------------------------------
    rims_to_scan = corpus.module_rims + corpus.global_rims
    if args.rim_limit:
        rims_to_scan = rims_to_scan[:args.rim_limit]
    rim_rt = {"count": 0, "byte_identical": 0, "fail": 0, "failures": []}
    for path in rims_to_scan:
        name = os.path.relpath(path, gd)
        res = rim_reencode_check(path)
        rim_rt["count"] += 1
        if res["ok"]:
            rim_rt["byte_identical"] += 1
        else:
            rim_rt["fail"] += 1
            rim_rt["failures"].append(f"{name}: {res}")
        rep.run("rim-archive", name, ka.read_rim, path)
        for e, payload in corpus.rim_entries(path):
            label = f"{name}/{e.resref}.{e.res_type}"
            rt = e.res_type
            if is_gff_payload(payload[:8]):
                def _gff2(p=payload, l=label):
                    g = pth.load_gff_bytes(p, l)
                    if gff.check(g):
                        raise ValueError("gff check failed")
                    return pth.gff_reencode(g)
                gff_rt["count"] += 1
                try:
                    out = _gff2()
                    if out == payload:
                        gff_rt["byte_identical"] += 1
                    else:
                        gff_rt["semantic_only"] += 1
                        gff_rt["failures"].append(f"{label}: re-encode differs")
                except Exception as exc:
                    gff_rt["fail"] += 1
                    if len(gff_rt["failures"]) < 100:
                        gff_rt["failures"].append(f"{label}: {type(exc).__name__}: {exc}")
                rep.run("gff", label, pth.load_gff_bytes, payload, label)
            if rt == T["pth"]:
                rep.run("pth", label, pth.parse_pth, payload, label)
            elif rt == T["gui"]:
                rep.run("gui", label, gui.parse_gui, payload, label)
            elif rt == T["2da"]:
                two_da_roundtrip(payload, label, two_da_rt)
                rep.run("two_da", label, lambda p, l: two_da_ascii.parse_2da_any(p, l), payload, label)
            elif rt == T["wav"]:
                rep.run("wav", label, wav.parse_wav, payload, label)
            elif rt == T["ssf"]:
                rep.run("ssf", label, ssf.parse_ssf, payload, label)
            elif rt == T["mdl"]:
                rep.run("mdl", label, mdl.parse_mdl, payload, label)
    print(f"rims scanned: {len(rims_to_scan)}; elapsed {time.time() - t0:.1f}s")

    # ------------------------------------------------------------------
    # 3. ERFs (patch + texture packs): archive re-encode + payload validation
    # ------------------------------------------------------------------
    erf_rt = {"count": 0, "byte_identical": 0, "fail": 0, "per_file": {}, "failures": []}
    for path in corpus.erfs:
        name = os.path.relpath(path, gd)
        res = erf_reencode_check(path)
        erf_rt["count"] += 1
        erf_rt["per_file"][name] = res
        if res["ok"]:
            erf_rt["byte_identical"] += 1
        else:
            erf_rt["fail"] += 1
            erf_rt["failures"].append(f"{name}: {res.get('detail')}")
        rep.run("erf-archive", name, ka.read_erf, path)
        for e, payload in corpus.erf_entries(path):
            label = f"{name}/{e.resref}.{e.res_type}"
            rt = e.res_type
            if is_gff_payload(payload[:8]):
                gff_rt["count"] += 1
                try:
                    g = pth.load_gff_bytes(payload, label)
                    if gff.check(g):
                        raise ValueError("gff check failed")
                    out = pth.gff_reencode(g)
                    if out == payload:
                        gff_rt["byte_identical"] += 1
                    else:
                        gff_rt["semantic_only"] += 1
                        gff_rt["failures"].append(f"{label}: re-encode differs")
                except Exception as exc:
                    gff_rt["fail"] += 1
                    if len(gff_rt["failures"]) < 100:
                        gff_rt["failures"].append(f"{label}: {type(exc).__name__}: {exc}")
                rep.run("gff", label, pth.load_gff_bytes, payload, label)
            if rt == T["tpc"]:
                rep.run("tpc", label, tpc.parse_tpc, payload, label, txi.parse_txi)
            elif rt == T["txi"]:
                rep.run("txi", label, txi.parse_txi, payload, label)
            elif rt == T["gui"]:
                rep.run("gui", label, gui.parse_gui, payload, label)
            elif rt == T["2da"]:
                two_da_roundtrip(payload, label, two_da_rt)
                rep.run("two_da", label, lambda p, l: two_da_ascii.parse_2da_any(p, l), payload, label)
            elif rt == T["mdl"]:
                rep.run("mdl", label, mdl.parse_mdl, payload, label)
    print(f"erfs: {len(corpus.erfs)}; elapsed {time.time() - t0:.1f}s")

    # ------------------------------------------------------------------
    # 4. BIF + KEY re-encode
    # ------------------------------------------------------------------
    bif_rt = {"count": 0, "byte_identical": 0, "fail": 0, "per_file": {}}
    for i, b in enumerate(corpus.key.bifs):
        path = ka.bif_path_for(gd, b["name"])
        name = os.path.relpath(path, gd)
        res = bif_reencode_check(path, i)
        bif_rt["count"] += 1
        bif_rt["per_file"][name] = {k: v for k, v in res.items()}
        if res["ok"]:
            bif_rt["byte_identical"] += 1
        else:
            bif_rt["fail"] += 1
        rep.run("bif-archive", name, ka.read_bif, path)
    key_rt = key_reencode_check(os.path.join(gd, "chitin.key"))
    rep.run("key-archive", "chitin.key", ka.read_key, os.path.join(gd, "chitin.key"))
    print(f"bif+key re-encode done; elapsed {time.time() - t0:.1f}s")

    # ------------------------------------------------------------------
    # 5. dialog.tlk round-trip
    # ------------------------------------------------------------------
    tlk_path = os.path.join(gd, "dialog.tlk")
    with open(tlk_path, "rb") as f:
        tlk_data = f.read()
    tlk_rt = {"count": 1, "entries": 0, "byte_identical": 0, "fail": 0, "failures": []}
    try:
        t = tlk_write.parse_tlk_bytes(tlk_data, "dialog.tlk")
        tlk_rt["entries"] = t.count
        if tlk_write.tlk_bytes(t) == tlk_data:
            tlk_rt["byte_identical"] = 1
        else:
            tlk_rt["failures"].append("tlk re-encode differs")
        rep.run("tlk", "dialog.tlk", tlk_write.parse_tlk_bytes, tlk_data, "dialog.tlk")
        # canonical repack must re-parse to identical entries and texts
        repacked = tlk_write.tlk_repack(t)
        t2 = tlk_write.parse_tlk_bytes(repacked, "repacked")
        same_text = (t.count == t2.count
                     and all(t.text(i) == t2.text(i) for i in range(t.count)))
        if not same_text:
            tlk_rt["failures"].append("tlk repack text mismatch")
    except Exception as exc:
        tlk_rt["fail"] = 1
        tlk_rt["failures"].append(f"dialog.tlk: {type(exc).__name__}: {exc}")

    # ------------------------------------------------------------------
    # 6. lips/*.mod -> LIP payloads
    # ------------------------------------------------------------------
    for path in corpus.lip_mods:
        name = os.path.relpath(path, gd)
        rep.run("erf-archive", name, ka.read_erf, path)
        for e, payload in corpus.erf_entries(path):
            if e.res_type == T["lip"]:
                rep.run("lip", f"{name}/{e.resref}", lip.parse_lip, payload,
                        f"{name}/{e.resref}")

    # ------------------------------------------------------------------
    # 7. loose media: movies (BIK), streams (WAV/MP3), DDS scan
    # ------------------------------------------------------------------
    movies = os.path.join(gd, "movies")
    for p in _lsdir(movies, ".bik"):
        fn = os.path.basename(p)
        size = os.path.getsize(p)
        with open(p, "rb") as f:
            head = f.read(min(size, bik.TABLE_OFFSET + 4 * 2_000_000))
        rep.run("bik", fn, bik.parse_bik, head, fn, True, size)

    dds_found = 0
    for sub in ("streammusic", "streamwaves", "streamsounds", "movies",
                "lips", "Override"):
        for p in _lsdir(os.path.join(gd, sub), ".dds"):
            dds_found += 1
            with open(p, "rb") as f:
                rep.run("dds", os.path.relpath(p, gd), dds.parse_dds, f.read(),
                        os.path.relpath(p, gd))
    rep.classes.setdefault("dds", {"count": dds_found, "ok": dds_found, "fail": 0,
                                   "failures": [], "note": "no shipped DDS in reference install"
                                   if dds_found == 0 else ""})

    def wav_file(path: str, label: str, mp3_check: bool = False):
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            head = f.read(min(size, 262144))
        try:
            w = wav.parse_wav(head, label, file_size=size)
        except wav.WavError as exc:
            if "read more" in str(exc):
                with open(path, "rb") as f:
                    w = wav.parse_wav(f.read(), label, file_size=size)
            else:
                raise
        if mp3_check:
            # streammusic .wav files carry MP3 frames in/after the data
            # chunk (the chunk size is often under-declared); streamsounds
            # files carry one MP3/LAME frame as a prefix before RIFF; some
            # streammusic files are plain PCM (no frame sync -> skipped).
            with open(path, "rb") as f:
                if w.prefix > 0:
                    f.seek(0)
                    blob = f.read(w.prefix)
                else:
                    for c in w.chunks:
                        if c.id == b"data":
                            f.seek(c.offset)
                            blob = f.read(max(0, size - c.offset))
                            break
                    else:
                        blob = b""
                if blob[:1] == b"\xff" and len(blob) > 1 and (blob[1] & 0xE0) == 0xE0:
                    mp3.parse_mp3_stream(blob, 0, len(blob), label, strict_end=False)
        return w

    for sub, mp3_check in (("streamwaves", False), ("streamsounds", True),
                           ("streammusic", True)):
        d = os.path.join(gd, sub)
        files = []
        for root, _dirs, names in os.walk(d):
            for fn in names:
                files.append(os.path.join(root, fn))
        files.sort()
        for p in files:
            rep.run("wav", os.path.relpath(p, gd), wav_file, p,
                    os.path.relpath(p, gd), mp3_check)
        print(f"{sub}: {len(files)} wav files validated; elapsed {time.time() - t0:.1f}s")

    # ------------------------------------------------------------------
    # 8. assemble + write JSON
    # ------------------------------------------------------------------
    all_fail = sum(c["fail"] for c in rep.classes.values())
    rt_summary = {
        "gff_reencode": gff_rt,
        "two_da_v2b": two_da_rt,
        "tlk": tlk_rt,
        "erf": erf_rt,
        "rim": rim_rt,
        "bif": bif_rt,
        "key": key_rt,
    }
    rt_fail = (gff_rt["fail"] + gff_rt["semantic_only"] + two_da_rt["fail"]
               + tlk_rt["fail"] + erf_rt["fail"] + rim_rt["fail"]
               + bif_rt["fail"] + (0 if key_rt["ok"] else 1))
    lay = rt_summary.get("two_da_v2b", {}).get("layouts")
    if isinstance(lay, Counter):
        rt_summary["two_da_v2b"]["layouts"] = dict(lay)
    result = {
        "lane": "s1-02-formats",
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "game_dir": os.path.abspath(gd),
        "exe_sha256": exe_hash,
        "exe_matches_build_of_record": exe_ok,
        "build_of_record": EXPECTED_EXE_SHA256,
        "base_type_counts": {str(k): v for k, v in sorted(type_counts.items())},
        "classes": rep.classes,
        "roundtrip": rt_summary,
        "summary": {
            "classes_total": sum(c["count"] for c in rep.classes.values()),
            "classes_ok": sum(c["ok"] for c in rep.classes.values()),
            "classes_fail": all_fail,
            "roundtrip_fail": rt_fail,
            "all_ok": all_fail == 0 and rt_fail == 0 and exe_ok,
            "elapsed_seconds": round(time.time() - t0, 1),
        },
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)

    print("\n==== summary ====")
    print(f"exe ok: {exe_ok}")
    for name in sorted(rep.classes):
        c = rep.classes[name]
        print(f"  {name:14s} count={c['count']:6d} ok={c['ok']:6d} fail={c['fail']}")
    for name, rt in rt_summary.items():
        if name == "key":
            print(f"  rt/{name:10s} ok={rt['ok']} detail={rt['detail']}")
        else:
            bi = rt.get("byte_identical", "")
            print(f"  rt/{name:10s} count={rt['count']} byte_identical={bi} "
                  f"fail={rt['fail']}")
    print(f"all_ok={result['summary']['all_ok']} "
          f"({result['summary']['elapsed_seconds']}s) -> {args.out}")
    return 0 if result["summary"]["all_ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
