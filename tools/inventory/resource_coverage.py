#!/usr/bin/env python3
"""G1 resource coverage ledger (lane s1-01-coverage).

Enumerates EVERY resource of a user-owned KotOR 1.0.3.0 install (build SHA-256
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`), read-only:

* chitin.key -> 26 BIFs (25,836 resources)
* modules/*.rim (234 RIMs, 20,575 resources)
* rims/*.rim (12 global RIMs, 1,936 resources)
* patch.erf (97) and TexturePacks/*.erf (11,453)
* lips/*.mod (ERF containers holding LIP resources)
* loose data files: dialog.tlk, Override/, streamwaves/, streamsounds/,
  streammusic/, movies/

For each resource the ledger records: archive path, resref, type id + label,
payload size, payload SHA-256, payload magic, parser bucket, and parse status
(ok / failed / open).  Gate G1 requires ZERO unhandled types: every type id in
kotor_types.py maps to a real parser (tools/formats/gff.py, two_da.py, tlk.py,
tools/resources/kotor_archive.py) or to a header validator implemented in this
file for media/containers (wav, mp3, bik, tga, dds, tpc, txi, ssf, lip, lyt,
wok/dwk/pwk=bwm, pth, vis, mdl/mdx, gui, ncs, nss, ini, ltr).

Header-validator evidence (docs/recon/02_TECHNICAL_NOTES.md "Data formats",
verified against the install by resource_probe.py and lane probes):
* GFF: 56-byte header, magic "<TYPE> " + version "V3.2" (observed "ARE V3.2").
* NCS: "NCS V1.0" + 0x42 opcode + u32be total size = 13-byte header
  (observed: `4e43532056312e30 42 00000017` for a 23-byte script).
* SSF: "SSF V1.1" + 28 StrRefs (u32 table), no filename table.
* LIP: "LIP V1.0" + f32 duration + u32 frame count + count*(f32 time, u8 viseme);
  observed len == 16 + count*5 exactly (206 = 16 + 38*5).
* TPC: 128-byte header; u32 data size at 0 (0 = raw), f32 alpha-test at 4,
  u16 width/height at 0x08/0x0A, encoding u8 at 0x0C (1 gray, 2 RGB/DXT1,
  4 RGBA/DXT5, 0xC BGRA), mip count u8 at 0x0D; TXI text appended after mip data.
* MDL/MDX: no magic; first u32 must be 0; 12-byte header, offsets exclude it.
* BWM (WOK/DWK/PWK): "BWM V1.0" + u32 type at +8 + 16 u32 counts/offsets at
  +72 (vc, vo, fc, fo, ...); vertex block vc*12 at vo, face block fc*12 at fo.
* WAV: some SFX carry a ~470-byte prefix before RIFF; probe "RIFF....WAVE"
  within the first 1 KiB instead of trusting offset 0.
* BMU = MP3 with "BMU" prefix (else ID3 tag or 0xFFE frame sync). BIK = "BIK"
  + version letter (observed "BIKi").

Totals are cross-checked against docs/recon/01_ARTIFACTS.md and
docs/recon/06_MANIFEST.md (expected counts are cited in CROSS_CHECK_SPEC).

The game directory is only ever opened read-only; the install hash is verified
first and the run aborts on drift.  No game bytes are written to tracked files;
the ledger goes to the lane artifacts directory (gitignored).

Usage:
    python tools/inventory/resource_coverage.py \
        --game-dir "$KOTOR_GAME_DIR" \
        --out artifacts/lanes/s1-01-coverage/coverage.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import tempfile
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
for sub in ("tools/inventory", "tools/resources", "tools/formats"):
    p = str(ROOT / sub)
    if p not in sys.path:
        sys.path.insert(0, p)

import kotor_archive as ka  # noqa: E402
from kotor_types import TYPE_INFO, label  # noqa: E402
import gff as gff_mod  # noqa: E402
from two_da import parse_2da, TwoDAError  # noqa: E402

BUILD_SHA256 = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6"

# ---------------------------------------------------------------------------
# type -> parser bucket.  Gate G1: every id in kotor_types.TYPE_INFO must be
# present here (see unhandled_type_ids()).
# ---------------------------------------------------------------------------

GFF_TYPE_IDS = {
    2012, 2014, 2015, 2023, 2024, 2025, 2026, 2027, 2029, 2030, 2032, 2035,
    2038, 2040, 2042, 2044, 2047, 2051, 2056, 2058, 3003, 2037,
    # 2024 BTI / 2026 BTC carry GFF magic "BTI V3.2"/"BTC V3.2" (verified in this
    # install; lane 03's corpus includes them: docs/recon/07_GFF_CORPUS.md).
}

TYPE_BUCKET: dict[int, str] = {t: "gff" for t in GFF_TYPE_IDS}
TYPE_BUCKET.update({
    0: "reserved",
    3: "tga",
    4: "wav",
    7: "ini",
    8: "mp3",
    10: "txt",
    2002: "mdl",
    2009: "text",
    2010: "ncs",
    2011: "erf",
    2016: "bwm",
    2057: "erf",
    2017: "two_da",
    2018: "tlk",
    2022: "txi",
    2033: "dds",
    2036: "ltr",
    2052: "bwm",
    2053: "bwm",
    2060: "ssf",
    2063: "bik",
    3000: "lyt",
    3001: "vis",
    3002: "rim",
    3004: "lip",
    3007: "tpc",
    3008: "mdx",
    9997: "erf",
    9998: "bif",
    9999: "key",
})

# Expected totals, cited: docs/recon/01_ARTIFACTS.md section 3 and
# docs/recon/06_MANIFEST.md "Files by kind"/"Top-level locations".
CROSS_CHECK_SPEC = [
    ("key_bif_files", 26, "01_ARTIFACTS.md §3: 26 BIF archives"),
    ("key_resources", 25836, "01_ARTIFACTS.md §3: 25,836 base resources"),
    ("module_rim_files", 234, "01_ARTIFACTS.md §2/§3: 234 RIM modules"),
    ("module_resources", 20575, "01_ARTIFACTS.md §3: 20,575 module resources"),
    ("module_entry_bytes", 71421011, "01_ARTIFACTS.md §3: 71,421,011 bytes"),
    ("global_rim_files", 12, "01_ARTIFACTS.md §2/§3: 12 global RIMs"),
    ("global_rim_resources", 1936, "01_ARTIFACTS.md §3: 1,936 resources"),
    ("global_rim_entry_bytes", 38914383, "01_ARTIFACTS.md §3: 38,914,383 bytes"),
    ("patch_erf_entries", 97, "01_ARTIFACTS.md §3: patch.erf 97 entries"),
    ("texture_erf_files", 4, "06_MANIFEST.md: TexturePacks/ 4 files"),
    ("texture_erf_entries", 11453, "01_ARTIFACTS.md §3: 11,453 texture-pack entries"),
    ("loose_dialog_tlk", 1, "06_MANIFEST.md: talk_table 1 file"),
    ("loose_streamwaves", 13799, "06_MANIFEST.md: stream_voice 13,799 files"),
    ("loose_streamsounds", 970, "06_MANIFEST.md: stream_sound 970 files"),
    ("loose_streammusic", 119, "06_MANIFEST.md: stream_music 119 files"),
    ("loose_movies", 61, "06_MANIFEST.md: movie 61 files"),
    ("loose_lips_mods", 123, "06_MANIFEST.md: lip_sync 123 files"),
    ("loose_override", 0, "06_MANIFEST.md: Override/ empty"),
]

LOOSE_EXT_TYPE = {
    ".wav": 4, ".bik": 2063, ".mp3": 8, ".bmu": 8, ".tlk": 2018,
    ".mod": 2011, ".erf": 9997, ".sav": 2057, ".ini": 7, ".txt": 10,
}
LOOSE_DIRS = ("Override", "streamwaves", "streamsounds", "streammusic", "movies")
ARCHIVE_EXT = {".mod", ".erf", ".sav"}


# ---------------------------------------------------------------------------
# header validators (stdlib only).  Each returns (status, detail) with
# status in {"ok", "failed", "open"}.
# ---------------------------------------------------------------------------


def _u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def _u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def _text_ratio(data: bytes, sample: int = 65536) -> float:
    s = data[:sample]
    if not s:
        return 0.0
    good = sum(1 for b in s if b in (9, 10, 13) or 32 <= b < 127 or b >= 0x80)
    return good / len(s)


def v_gff(data: bytes, rtype: int, tmp: _TmpFile) -> tuple[str, str]:
    """Real parser: tools/formats/gff.py (Gff + structural check)."""
    try:
        path = tmp.write(data)
        g = gff_mod.Gff(path)
        if g.version != b"V3.2":
            return "failed", f"GFF version {g.version!r} != V3.2"
        want = label(rtype).encode("latin-1")
        if rtype != 2037 and g.magic.rstrip(b" ") != want:
            return "failed", f"GFF magic {g.magic!r} != type label {want!r}"
        errs = gff_mod.check(g)
        if errs:
            return "failed", "; ".join(errs[:3])
        return "ok", (f"magic={g.magic!r} structs={g.struct_cnt} fields={g.field_cnt} "
                       f"labels={g.label_cnt}")
    except Exception as exc:
        return "failed", f"{type(exc).__name__}: {exc}"


def _v2b_header_fallback(data: bytes) -> str:
    """Header validation for the NUL-separated V2.b variant shipped inside
    rims/global.rim and rims/miniglobal.rim (column names and row labels are
    NUL-separated and the column blob ends with a double NUL, instead of the
    TAB-separated form in data/2da.bif).  Validates magic, column blob,
    row count, label table, cell-offset table and string table ranges.
    Returns a detail string; raises ValueError when structurally invalid."""
    if data[:8] != b"2DA V2.b":
        raise ValueError("bad V2.b signature")
    pos = 8
    if data[pos:pos + 1] == b"\r":
        pos += 1
    if data[pos:pos + 1] != b"\n":
        raise ValueError("V2.b header not newline-terminated")
    pos += 1
    end = data.find(b"\x00\x00", pos)
    if end < 0:
        raise ValueError("unterminated column blob")
    cols = data[pos:end].split(b"\x00")
    if not cols or any(not c for c in cols) or any(b"\t" in c for c in cols):
        raise ValueError("bad column names")
    pos = end + 2
    if pos + 4 > len(data):
        raise ValueError("no row count")
    row_count = _u32(data, pos)
    pos += 4
    if row_count > 1_000_000:
        raise ValueError(f"implausible row count {row_count}")
    cell_count = row_count * len(cols)
    # labels: try both "every label separator-terminated" and "last label
    # unterminated" alignments; the offset table must validate either way.
    for term_last in (True, False):
        p = pos
        ok = True
        for i in range(row_count):
            e = data.find(b"\x00", p)
            if e < 0 or e == p:
                ok = False
                break
            p = e + 1
            if not term_last and i == row_count - 1:
                p = e  # last label not terminated
        if not ok or p + cell_count * 2 + 2 > len(data):
            continue
        offs = struct.unpack_from(f"<{cell_count}H", data, p) if cell_count else ()
        ssize = _u16(data, p + cell_count * 2)
        if p + cell_count * 2 + 2 + ssize > len(data):
            continue
        if any(o >= ssize for o in offs):
            continue
        return (f"columns={len(cols)} rows={row_count} "
                f"(NUL-separated V2.b variant, header-valid)")
    raise ValueError("row label / cell-offset tables do not validate")


def v_two_da(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    try:
        t = parse_2da(data)
        return "ok", f"columns={len(t.columns)} rows={len(t.rows)}"
    except TwoDAError as exc:
        # The strict parser requires TAB-separated column names; some shipped
        # RIM copies use the NUL-separated variant.  Fall back to a structural
        # header validation (magic + all table ranges) before failing.
        try:
            return "ok", f"{_v2b_header_fallback(data)}; strict parser: {exc}"
        except ValueError as exc2:
            return "failed", f"{exc}; fallback: {exc2}"
    except Exception as exc:
        return "failed", f"{type(exc).__name__}: {exc}"


def v_tlk(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    """tools/formats/tlk.py wire layout: TLK V3.0 + lang/count/textOff + 40B entries."""
    try:
        if len(data) < 20:
            return "failed", "shorter than 20-byte TLK header"
        sig, ver, lang, count, text_off = struct.unpack_from("<4s4sIII", data, 0)
        if sig != b"TLK " or ver != b"V3.0":
            return "failed", f"bad TLK signature {sig!r} {ver!r}"
        if 20 + count * 40 > len(data):
            return "failed", f"entry table {count}*40 exceeds file"
        if text_off > len(data):
            return "failed", "text offset outside file"
        bad = 0
        for i in range(count):
            off, length = struct.unpack_from("<II", data, 20 + i * 40 + 24)
            if off + length > len(data) - text_off:
                bad += 1
        if bad:
            return "failed", f"{bad}/{count} entries with text outside blob"
        return "ok", f"language={lang} entries={count} text_off={text_off}"
    except Exception as exc:
        return "failed", f"{type(exc).__name__}: {exc}"


def v_ncs(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    """NCS V1.0, big-endian 13-byte header: sig(8) + 0x42 opcode + u32be size."""
    if len(data) < 13:
        return "failed", "shorter than 13-byte NCS header"
    if data[:8] != b"NCS V1.0":
        return "failed", f"bad signature {data[:8]!r}"
    if data[8] != 0x42:
        return "failed", f"header opcode {data[8]:#x} != 0x42"
    size = struct.unpack_from(">I", data, 9)[0]
    if size != len(data):
        return "failed", f"header size {size} != file size {len(data)}"
    return "ok", f"be size={size}"


def v_text(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    if not data:
        return "failed", "empty text payload"
    if _text_ratio(data) < 0.95:
        return "failed", f"printable ratio {_text_ratio(data):.3f} < 0.95"
    return "ok", "text"


def v_txi(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    # TXI is key/value text; some shipped files open with a numeric line
    # (e.g. filmnoisetex starts with "0.02"), so accept any printable line
    # of the form `token` or `token value...`.
    if not data or _text_ratio(data) < 0.99:
        return "failed", "TXI not text"
    for line in data.split(b"\n"):
        line = line.strip(b"\r ")
        if not line:
            continue
        if not all(32 <= b < 127 for b in line):
            return "failed", f"non-printable TXI line {line[:32]!r}"
    return "ok", "key/value text"


def v_lyt(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    """#MAXLAYOUT ASCII grammar (observed on shipped m01aa/m45mg/mgf_ebonhawk):
    `#MAXLAYOUT ASCII`, `filedependancy <file>`, `beginlayout`, then ordered
    sections `<name>count <n>` followed by exactly n entry lines -- room/track/
    obstacle entries are `<name> <x> <y> <z>`, door-hook entries are
    `<room> <door> <n> <8 numbers>` -- and `donelayout`."""
    if not data.startswith(b"#MAXLAYOUT"):
        return "failed", "missing #MAXLAYOUT magic"
    if _text_ratio(data) < 0.99:
        return "failed", "LYT not text"
    pending: dict[str, int] = {}
    order: list[str] = []
    for line in data.split(b"\n")[1:]:
        line = line.strip(b"\r ")
        if not line:
            continue
        parts = line.split()
        if len(parts) == 1:
            if pending:
                return "failed", f"section {order[-1]} unfinished at {line[:24]!r}"
            continue  # beginlayout / donelayout / ...
        if len(parts) == 2 and (parts[0].endswith(b"count") or parts[0] == b"filedependancy"):
            if pending:
                return "failed", f"section {order[-1]} unfinished before {parts[0]!r}"
            if parts[0].endswith(b"count"):
                try:
                    n = int(parts[1])
                except ValueError:
                    return "failed", f"non-integer count in {line[:32]!r}"
                if n < 0:
                    return "failed", f"negative count in {line[:32]!r}"
                if n:
                    pending[parts[0][:-5].decode("latin-1")] = n
                    order.append(parts[0][:-5].decode("latin-1"))
            continue
        if not pending:
            return "failed", f"entry line before any count: {line[:32]!r}"
        section = order[-1]
        if section == "doorhook":
            if len(parts) < 5:
                return "failed", f"short doorhook line {line[:32]!r}"
            try:
                [float(p) for p in parts[2:]]
            except ValueError:
                return "failed", f"non-numeric doorhook line {line[:32]!r}"
        else:
            if len(parts) != 4:
                return "failed", f"bad {section} line {line[:32]!r}"
            try:
                [float(p) for p in parts[1:]]
            except ValueError:
                return "failed", f"non-float {section} line {line[:32]!r}"
        pending[section] -= 1
        if pending[section] == 0:
            del pending[section]
    if pending:
        return "failed", f"unfinished sections {pending}"
    return "ok", f"sections={order}"


def v_vis(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    if not data or _text_ratio(data) < 0.99:
        return "failed", "VIS not text"
    pairs = 0
    for line in data.split(b"\n"):
        line = line.strip(b"\r ")
        if not line:
            continue
        parts = line.split()
        if not 1 <= len(parts) <= 2:
            return "failed", f"bad VIS line {line[:32]!r}"
        pairs += 1
    return "ok", f"lines={pairs}"


def v_ssf(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    """KotOR SSF V1.1: sig + 28 StrRefs (u32 each), no filename table."""
    if len(data) < 8 or data[:8] != b"SSF V1.1":
        return "failed", f"bad SSF signature {data[:8]!r}"
    if len(data) < 8 + 28 * 4:
        return "failed", f"shorter than 8+28*4=120 bytes ({len(data)})"
    if (len(data) - 8) % 4:
        return "failed", "StrRef table not u32-aligned"
    return "ok", f"strref table bytes={len(data) - 8}"


def v_lip(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    """LIP V1.0 + f32 duration + u32 frame count + count*(f32 time, u8 viseme)."""
    if len(data) < 16 or data[:8] != b"LIP V1.0":
        return "failed", f"bad LIP signature {data[:8]!r}"
    count = _u32(data, 12)
    if len(data) != 16 + count * 5:
        return "failed", f"len {len(data)} != 16 + {count}*5"
    for i in range(count):
        viseme = data[16 + i * 5 + 4]
        if viseme > 15:
            return "failed", f"viseme {viseme} > 15 at frame {i}"
    return "ok", f"frames={count} duration={struct.unpack_from('<f', data, 8)[0]:.3f}"


def v_bwm(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    """BWM V1.0 + u32 type@8 + 16 u32 counts/offsets@72 (vc,vo,fc,fo,...)."""
    if len(data) < 136 or data[:8] != b"BWM V1.0":
        return "failed", f"bad BWM signature {data[:8]!r}"
    btype = _u32(data, 8)
    vc, vo, fc, fo = struct.unpack_from("<4I", data, 72)
    if vc and (vo >= len(data) or vo + vc * 12 > len(data)):
        return "failed", f"vertex block vo={vo} vc={vc} exceeds {len(data)}"
    if fc and (fo >= len(data) or fo + fc * 12 > len(data)):
        return "failed", f"face block fo={fo} fc={fc} exceeds {len(data)}"
    if btype == 1 and fc:
        no = _u32(data, 72 + 5 * 4)
        if no and (no + fc * 12 > len(data)):
            return "failed", f"normal block no={no} fc={fc} exceeds {len(data)}"
    return "ok", f"bwm_type={btype} verts={vc} faces={fc}"


def v_mdl(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    """MDL: no magic; first u32 == 0; 12-byte header, offsets exclude it.
    Name at +20 (32B), root offset u32@52, mdx size/offset u32@188/@192."""
    if len(data) < 56:
        return "failed", f"shorter than 56 bytes ({len(data)})"
    if _u32(data, 0) != 0:
        return "failed", f"first u32 {_u32(data, 0):#x} != 0"
    root = _u32(data, 52)
    if root and 12 + root + 20 > len(data):
        return "failed", f"root offset {root} (+12 header) exceeds file"
    if len(data) >= 196:
        mdx_size, mdx_off = struct.unpack_from("<2I", data, 188)
        if mdx_size and mdx_off and 12 + mdx_off + mdx_size > len(data):
            return "failed", (f"mdx block {mdx_off}+{mdx_size} (+12) exceeds "
                              f"{len(data)}")
    return "ok", f"root={root}"


def v_mdx(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    """MDX carries no magic and no file header: recon/15_MDX_LAYOUT.md verifies
    the mesh blocks start at byte 0 and the last block ends exactly at EOF
    (block strides come from the paired MDL).  Header-level check: the payload
    is u32-aligned (empty MDX payloads exist)."""
    if len(data) % 4:
        return "failed", f"size {len(data)} not u32-aligned"
    if not data:
        return "ok", "empty MDX (no vertex blocks)"
    return "ok", f"block stream, {len(data)} bytes, stride from paired MDL"


def v_tga(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    """18-byte Targa header sanity; exact-size check only for uncompressed
    types where KotOR is exact -- observed chrome1.tga carries 26 trailing
    bytes, so only a lower bound is enforced."""
    if len(data) < 18:
        return "failed", f"shorter than 18-byte TGA header ({len(data)})"
    idlen, cmaptype, dtype = data[0], data[1], data[2]
    cmap_len, cmap_depth = _u16(data, 5), data[7]
    w, h, bpp = _u16(data, 12), _u16(data, 14), data[16]
    if cmaptype not in (0, 1):
        return "failed", f"colormap type {cmaptype}"
    if dtype not in (1, 2, 3, 9, 10, 11, 32, 33):
        return "failed", f"datatype {dtype}"
    if not (0 < w <= 16384 and 0 < h <= 16384):
        return "failed", f"dimensions {w}x{h}"
    if bpp not in (8, 15, 16, 24, 32):
        return "failed", f"bpp {bpp}"
    cmap_bytes = cmap_len * cmap_depth // 8 if cmaptype == 1 else 0
    if dtype in (1, 2, 3) and len(data) < 18 + idlen + cmap_bytes + w * h * bpp // 8:
        return "failed", (f"len {len(data)} < 18+{idlen}+{cmap_bytes}+"
                          f"{w}x{h}x{bpp}/8")
    if len(data) < 18 + idlen + cmap_bytes + w * h:
        return "failed", f"len {len(data)} below 1-byte/px floor for {w}x{h}"
    return "ok", f"{w}x{h} bpp={bpp} dtype={dtype}"


def v_wav(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    """Probe RIFF within the first 1 KiB (some SFX carry a ~470-byte prefix)."""
    off = data.find(b"RIFF", 0, 1024)
    if off < 0:
        return "failed", "no RIFF signature in first 1 KiB"
    if len(data) < off + 12 or data[off + 8:off + 12] != b"WAVE":
        return "failed", "RIFF without WAVE"
    riff_size = _u32(data, off + 4)
    if off + 8 + riff_size > len(data):
        return "failed", f"RIFF size {riff_size} exceeds file from {off}"
    end = off + 8 + (riff_size or len(data) - off - 8)
    if data.find(b"fmt ", off + 12, min(end, off + 12 + 65536)) < 0:
        return "failed", "no fmt chunk"
    return "ok", f"riff@{off} size={riff_size}"


def v_mp3(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    if data[:3] == b"BMU" or data[:3] == b"ID3":
        return "ok", data[:3].decode("latin-1")
    if len(data) >= 2 and data[0] == 0xFF and data[1] & 0xE0 == 0xE0:
        return "ok", "frame sync"
    return "failed", f"no BMU/ID3/frame-sync in {data[:8].hex()}"


def v_bik(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    if len(data) >= 4 and data[:3] == b"BIK" and 0x61 <= data[3] <= 0x7A:
        return "ok", f"Bink version {chr(data[3])}"
    if data[:3] in (b"KB1", b"KB2"):
        return "ok", data[:3].decode("latin-1")
    return "failed", f"bad BIK signature {data[:4]!r}"


def v_dds(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    if len(data) < 128 or data[:4] != b"DDS ":
        return "failed", f"bad DDS signature {data[:4]!r}"
    if _u32(data, 4) != 124:
        return "failed", f"DDS header size {_u32(data, 4)} != 124"
    return "ok", f"flags={_u32(data, 8):#x} height={_u32(data, 12)} width={_u32(data, 16)}"


def v_tpc(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    """128-byte TPC header; TXI appended after mip data (offset computed)."""
    if len(data) < 128:
        return "failed", f"shorter than 128-byte TPC header ({len(data)})"
    data_size = _u32(data, 0)
    alpha = struct.unpack_from("<f", data, 4)[0]
    w, h = _u16(data, 8), _u16(data, 0xA)
    enc, mips = data[0xC], data[0xD]
    if not (1 <= w <= 8192 and 1 <= h <= 8192):
        return "failed", f"dimensions {w}x{h}"
    if enc not in (0, 1, 2, 4, 0xC):
        return "failed", f"encoding {enc:#x}"
    if not 0 <= mips <= 16:
        return "failed", f"mip count {mips}"
    if data_size and 128 + data_size > len(data):
        return "failed", f"data size {data_size} + 128 exceeds {len(data)}"
    if not 0.0 <= alpha <= 1.0:
        return "failed", f"alpha-test {alpha}"
    txi = len(data) - 128 - data_size if data_size else 0
    return "ok", f"{w}x{h} enc={enc:#x} mips={mips} data={data_size} txi_tail={txi}"


def v_ini(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    if not data or _text_ratio(data) < 0.95:
        return "failed", "INI not text"
    sections = 0
    for line in data.split(b"\n"):
        line = line.strip(b"\r ")
        if not line or line.startswith((b";", b"#")):
            continue
        if line.startswith(b"[") and line.endswith(b"]"):
            sections += 1
        elif b"=" not in line:
            return "failed", f"bad INI line {line[:32]!r}"
    if not sections:
        return "failed", "no [section]"
    return "ok", f"sections={sections}"


def v_ltr(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    if len(data) < 12 or data[:8] != b"LTR V1.0":
        return "failed", f"bad LTR signature {data[:8]!r}"
    return "ok", f"count={_u32(data, 8)}"


def v_erf(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    try:
        a = ka.read_erf_bytes(data)
        return "ok", f"{a.format} entries={len(a.entries)} layout=kotor-v1"
    except ka.ArchiveError as exc:
        return "failed", str(exc)


def v_rim(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    try:
        path = tmp.write(data)
        a = ka.read_rim(path)
        return "ok", f"entries={len(a.entries)}"
    except (ka.ArchiveError, OSError) as exc:
        return "failed", str(exc)


def v_bif(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    try:
        path = tmp.write(data)
        b = ka.read_bif(path)
        return "ok", f"records={len(b.records)}"
    except (ka.ArchiveError, OSError) as exc:
        return "failed", str(exc)


def v_key(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    try:
        path = tmp.write(data)
        k = ka.read_key(path)
        return "ok", f"bifs={len(k.bifs)} entries={len(k.entries)}"
    except (ka.ArchiveError, OSError) as exc:
        return "failed", str(exc)


def v_probe(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    """Fallback for layouts without a decoded spec: record magic and mark
    open (triaged in docs/recon/17_RESOURCE_COVERAGE.md)."""
    if not data:
        return "failed", "empty payload"
    return "open", f"no layout validator; magic={data[:8].hex()}"


def v_reserved(data: bytes, rtype: int, tmp: "_TmpFile") -> tuple[str, str]:
    return "failed", "type 0 is reserved/invalid"


VALIDATORS = {
    "gff": v_gff, "two_da": v_two_da, "tlk": v_tlk, "ncs": v_ncs,
    "text": v_text, "txt": v_text, "txi": v_txi, "lyt": v_lyt, "vis": v_vis,
    "ssf": v_ssf, "lip": v_lip, "bwm": v_bwm, "mdl": v_mdl, "mdx": v_mdx,
    "tga": v_tga, "wav": v_wav, "mp3": v_mp3, "bik": v_bik, "dds": v_dds,
    "tpc": v_tpc, "ini": v_ini, "ltr": v_ltr, "erf": v_erf, "rim": v_rim,
    "bif": v_bif, "key": v_key, "probe": v_probe, "reserved": v_reserved,
}


class _TmpFile:
    """One reusable scratch file for path-based parsers (gff/archive readers)."""

    def __init__(self):
        fd, self.path = tempfile.mkstemp(prefix="s1_01_cov_", suffix=".bin")
        os.close(fd)

    def write(self, data: bytes) -> str:
        with open(self.path, "wb") as f:
            f.write(data)
        return self.path

    def close(self):
        try:
            os.unlink(self.path)
        except OSError:
            pass


def unhandled_type_ids() -> list[int]:
    """Gate G1: type ids in kotor_types.py without a parser bucket."""
    return sorted(t for t in TYPE_INFO if t not in TYPE_BUCKET)


def classify(payload: bytes, rtype: int, tmp: _TmpFile) -> tuple[str, str, str]:
    """Return (bucket, status, detail) for one payload."""
    bucket = TYPE_BUCKET.get(rtype)
    if bucket is None:
        return "unhandled", "open", f"type {rtype} not in TYPE_BUCKET"
    status, detail = VALIDATORS[bucket](payload, rtype, tmp)
    return bucket, status, detail


# ---------------------------------------------------------------------------
# enumeration (read-only against the game dir)
# ---------------------------------------------------------------------------


def normalize_game_dir(game_dir: str) -> str:
    """Accept MSYS (/e/...) or Windows (E:/...) spellings of the install root."""
    if os.path.isdir(game_dir):
        return game_dir
    if len(game_dir) >= 3 and game_dir[0] == "/" and game_dir[2] == "/":
        cand = game_dir[1].upper() + ":/" + game_dir[3:]
        if os.path.isdir(cand):
            return cand
    raise SystemExit(f"game dir not found: {game_dir}")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def verify_exe(game_dir: str) -> dict:
    exe = os.path.join(game_dir, "swkotor.exe")
    h = hashlib.sha256()
    with open(exe, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    digest = h.hexdigest()
    return {
        "path": "swkotor.exe",
        "size": os.path.getsize(exe),
        "sha256": digest,
        "matches_artifact_of_record": digest == BUILD_SHA256,
    }


def make_record(source, archive, entry_index, resref, rtype, payload, tmp):
    bucket, status, detail = classify(payload, rtype, tmp)
    return {
        "source": source,
        "archive": archive,
        "entry": entry_index,
        "resref": resref,
        "type": rtype,
        "type_label": label(rtype),
        "size": len(payload),
        "sha256": sha256_bytes(payload),
        "magic": payload[:16].hex(),
        "bucket": bucket,
        "status": status,
        "detail": detail,
    }


def container_row(game_dir: str, rel: str) -> dict:
    path = os.path.join(game_dir, rel)
    return {
        "path": rel,
        "size": os.path.getsize(path),
        "sha256": ka.sha256_file(path),
    }


def enumerate_key_bifs(game_dir, resources, containers, tmp, log):
    key = ka.read_key(os.path.join(game_dir, "chitin.key"))
    containers.append(container_row(game_dir, "chitin.key"))
    by_bif: dict[int, list] = {}
    for ke in key.entries:
        by_bif.setdefault(ke.bif_index, []).append(ke)
    for meta in key.bifs:
        rel = ka.bif_path_for(game_dir, meta["name"])
        rel_name = os.path.relpath(rel, game_dir).replace(os.sep, "/")
        containers.append(container_row(game_dir, rel_name))
        bif = ka.read_bif(rel)
        rows = bif.records  # var table then fixed table, in order
        with open(rel, "rb") as f:
            for ke in by_bif.get(meta["index"], []):
                if ke.table_index >= len(rows):
                    resources.append({
                        "source": "key", "archive": rel_name, "entry": ke.table_index,
                        "resref": ke.resref, "type": ke.res_type,
                        "type_label": label(ke.res_type), "size": 0, "sha256": "",
                        "magic": "", "bucket": TYPE_BUCKET.get(ke.res_type, "unhandled"),
                        "status": "failed",
                        "detail": f"table index {ke.table_index} >= {len(rows)} records",
                    })
                    continue
                rec = rows[ke.table_index]
                f.seek(rec.offset)
                payload = f.read(rec.size)
                if len(payload) != rec.size:
                    payload = b""
                    resources.append({
                        "source": "key", "archive": rel_name, "entry": ke.table_index,
                        "resref": ke.resref, "type": ke.res_type,
                        "type_label": label(ke.res_type), "size": rec.size, "sha256": "",
                        "magic": "", "bucket": TYPE_BUCKET.get(ke.res_type, "unhandled"),
                        "status": "failed", "detail": "short read in BIF",
                    })
                    continue
                resources.append(make_record("key", rel_name, ke.table_index,
                                             ke.resref, ke.res_type, payload, tmp))
        log(f"key: {rel_name} done ({len(by_bif.get(meta['index'], []))} resources)")
    return len(key.entries)


def enumerate_archives(game_dir, subdirs, resources, containers, tmp, log,
                       source_for, magic_ext=".rim"):
    counts = {}
    for sub in subdirs:
        d = os.path.join(game_dir, sub)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.lower().endswith(magic_ext):
                continue
            rel = f"{sub}/{fn}"
            containers.append(container_row(game_dir, rel))
            arch = ka.read_rim(os.path.join(game_dir, rel)) if magic_ext == ".rim" \
                else ka.read_erf(os.path.join(game_dir, rel))
            for e in arch.entries:
                payload = arch.read_entry(e)
                resources.append(make_record(source_for(sub, fn), rel, e.index,
                                             e.resref, e.res_type, payload, tmp))
            counts[rel] = len(arch.entries)
            log(f"{source_for.__name__}: {rel} entries={len(arch.entries)}")
    return counts


def enumerate_loose(game_dir, resources, containers, tmp, log):
    # dialog.tlk at the install root
    tlk_path = os.path.join(game_dir, "dialog.tlk")
    if os.path.isfile(tlk_path):
        containers.append(container_row(game_dir, "dialog.tlk"))
        with open(tlk_path, "rb") as f:
            payload = f.read()
        resources.append(make_record("loose", "dialog.tlk", 0, "dialog",
                                     2018, payload, tmp))
        log("loose: dialog.tlk done")
    counts = {}
    for sub in LOOSE_DIRS:
        d = os.path.join(game_dir, sub)
        if not os.path.isdir(d):
            continue
        n = 0
        for dirpath, _dirnames, filenames in os.walk(d):
            for fn in sorted(filenames):
                rel = os.path.relpath(os.path.join(dirpath, fn), game_dir).replace(os.sep, "/")
                ext = os.path.splitext(fn)[1].lower()
                with open(os.path.join(dirpath, fn), "rb") as f:
                    payload = f.read()
                if ext in ARCHIVE_EXT:
                    containers.append(container_row(game_dir, rel))
                    arch = ka.read_erf(os.path.join(dirpath, fn))
                    for e in arch.entries:
                        ep = arch.read_entry(e)
                        resources.append(make_record("loose_archive", rel, e.index,
                                                     e.resref, e.res_type, ep, tmp))
                    n += 1
                    continue
                rtype = LOOSE_EXT_TYPE.get(ext)
                if rtype is None:
                    # sniff: RIFF -> wav, BIK -> bik, else unknown
                    if payload.find(b"RIFF", 0, 1024) >= 0:
                        rtype = 4
                    elif payload[:3] == b"BIK":
                        rtype = 2063
                    else:
                        rtype = -1
                resources.append(make_record("loose", rel, 0,
                                             os.path.splitext(fn)[0], rtype,
                                             payload, tmp))
                n += 1
        counts[sub] = n
        log(f"loose: {sub} files={n}")
    return counts


# ---------------------------------------------------------------------------
# summary / cross-checks
# ---------------------------------------------------------------------------


def build_summary(resources, containers, exe_info, loose_counts, started,
                  cross_check_spec=CROSS_CHECK_SPEC):
    by_type: dict[str, dict] = {}
    by_bucket: dict[str, dict] = {}
    by_source: Counter = Counter()
    status_counts: Counter = Counter()
    total_bytes = 0
    failures = []
    opens = []
    observed_types = set()
    for r in resources:
        tkey = str(r["type"])
        observed_types.add(r["type"])
        bt = by_type.setdefault(tkey, {
            "label": r["type_label"], "count": 0, "bytes": 0,
            "ok": 0, "failed": 0, "open": 0, "bucket": r["bucket"],
        })
        bt["count"] += 1
        bt["bytes"] += r["size"]
        bt[r["status"]] = bt.get(r["status"], 0) + 1
        bb = by_bucket.setdefault(r["bucket"], {"count": 0, "bytes": 0, "ok": 0,
                                                 "failed": 0, "open": 0})
        bb["count"] += 1
        bb["bytes"] += r["size"]
        bb[r["status"]] = bb.get(r["status"], 0) + 1
        by_source[r["source"]] += 1
        status_counts[r["status"]] += 1
        total_bytes += r["size"]
        if r["status"] == "failed":
            failures.append(r)
        elif r["status"] == "open":
            opens.append(r)

    actual = {
        "key_bif_files": sum(1 for c in containers if c["path"].endswith(".bif")),
        "key_resources": by_source.get("key", 0),
        "module_rim_files": sum(1 for c in containers if c["path"].startswith("modules/")),
        "module_resources": by_source.get("module_rim", 0),
        "module_entry_bytes": sum(r["size"] for r in resources if r["source"] == "module_rim"),
        "global_rim_files": sum(1 for c in containers if c["path"].startswith("rims/")),
        "global_rim_resources": by_source.get("global_rim", 0),
        "global_rim_entry_bytes": sum(r["size"] for r in resources if r["source"] == "global_rim"),
        "patch_erf_entries": by_source.get("patch_erf", 0),
        "texture_erf_files": sum(1 for c in containers if c["path"].startswith("TexturePacks/")),
        "texture_erf_entries": by_source.get("texture_erf", 0),
        "loose_dialog_tlk": 1 if by_source.get("loose", 0) and any(
            r["archive"] == "dialog.tlk" for r in resources) else 0,
        "loose_streamwaves": loose_counts.get("streamwaves", 0),
        "loose_streamsounds": loose_counts.get("streamsounds", 0),
        "loose_streammusic": loose_counts.get("streammusic", 0),
        "loose_movies": loose_counts.get("movies", 0),
        "loose_lips_mods": sum(1 for c in containers if c["path"].startswith("lips/")),
        "loose_override": loose_counts.get("Override", 0),
    }
    cross_checks = []
    for name, expected, cite in cross_check_spec:
        got = actual.get(name)
        cross_checks.append({
            "name": name, "expected": expected, "actual": got,
            "pass": got == expected, "evidence": cite,
        })
    unknown_observed = sorted(t for t in observed_types if t not in TYPE_INFO and t != -1)
    return {
        "generated_utc": started,
        "game_dir": None,  # filled by caller
        "build_sha256": BUILD_SHA256,
        "exe": exe_info,
        "totals": {
            "resources": len(resources),
            "containers": len(containers),
            "resource_bytes": total_bytes,
            "by_source": dict(by_source),
            "by_status": dict(status_counts),
        },
        "by_type": dict(sorted(by_type.items(), key=lambda kv: int(kv[0]))),
        "by_bucket": dict(sorted(by_bucket.items())),
        "gate_g1": {
            "unhandled_type_ids_in_kotor_types": unhandled_type_ids(),
            "observed_type_ids_not_in_kotor_types": unknown_observed,
            "zero_unhandled": not unhandled_type_ids() and not unknown_observed,
            "failed_count": status_counts.get("failed", 0),
            "open_count": status_counts.get("open", 0),
        },
        "cross_checks": cross_checks,
        "cross_checks_pass": all(c["pass"] for c in cross_checks),
        "failures": failures,
        "open_items": opens,
    }


def run(game_dir: str, out_path: str, log=print, verify_build: bool = True,
        cross_check_spec=CROSS_CHECK_SPEC) -> dict:
    game_dir = normalize_game_dir(game_dir)
    started = datetime.now(timezone.utc).isoformat()
    if verify_build:
        exe_info = verify_exe(game_dir)
        if not exe_info["matches_artifact_of_record"]:
            raise SystemExit(
                f"install drift: swkotor.exe sha256 {exe_info['sha256']} "
                f"!= {BUILD_SHA256}; stop")
    else:
        exe_info = {"path": "swkotor.exe", "size": None, "sha256": None,
                    "matches_artifact_of_record": None,
                    "note": "synthetic fixture tree; build hash not checked"}
    tmp = _TmpFile()
    resources: list[dict] = []
    containers: list[dict] = []
    try:
        key_count = enumerate_key_bifs(game_dir, resources, containers, tmp, log)
        enumerate_archives(game_dir, ["modules"], resources, containers, tmp, log,
                           lambda sub, fn: "module_rim", ".rim")
        enumerate_archives(game_dir, ["rims"], resources, containers, tmp, log,
                           lambda sub, fn: "global_rim", ".rim")
        # patch.erf at root + TexturePacks/*.erf
        p = os.path.join(game_dir, "patch.erf")
        if os.path.isfile(p):
            containers.append(container_row(game_dir, "patch.erf"))
            arch = ka.read_erf(p)
            for e in arch.entries:
                resources.append(make_record("patch_erf", "patch.erf", e.index,
                                             e.resref, e.res_type,
                                             arch.read_entry(e), tmp))
            log(f"patch_erf: patch.erf entries={len(arch.entries)}")
        tp = os.path.join(game_dir, "TexturePacks")
        if os.path.isdir(tp):
            for fn in sorted(os.listdir(tp)):
                if not fn.lower().endswith(".erf"):
                    continue
                rel = f"TexturePacks/{fn}"
                containers.append(container_row(game_dir, rel))
                arch = ka.read_erf(os.path.join(tp, fn))
                for e in arch.entries:
                    resources.append(make_record("texture_erf", rel, e.index, e.resref,
                                                 e.res_type, arch.read_entry(e), tmp))
                log(f"texture_erf: {rel} entries={len(arch.entries)}")
        # lips/*.mod are ERF containers of LIP resources
        lips = os.path.join(game_dir, "lips")
        if os.path.isdir(lips):
            for fn in sorted(os.listdir(lips)):
                if not fn.lower().endswith(".mod"):
                    continue
                rel = f"lips/{fn}"
                containers.append(container_row(game_dir, rel))
                arch = ka.read_erf(os.path.join(lips, fn))
                for e in arch.entries:
                    resources.append(make_record("lips_mod", rel, e.index, e.resref,
                                                 e.res_type, arch.read_entry(e), tmp))
                log(f"lips_mod: {rel} entries={len(arch.entries)}")
        loose_counts = enumerate_loose(game_dir, resources, containers, tmp, log)
    finally:
        tmp.close()
    summary = build_summary(resources, containers, exe_info, loose_counts,
                            started, cross_check_spec)
    summary["game_dir"] = os.path.abspath(game_dir)
    ledger = {
        "meta": {
            "tool": "tools/inventory/resource_coverage.py",
            "lane": "s1-01-coverage",
            "generated_utc": started,
            "game_dir": os.path.abspath(game_dir),
            "build_sha256": BUILD_SHA256,
            "exe_sha256": exe_info["sha256"],
            "read_only": True,
        },
        "summary": summary,
        "containers": containers,
        "resources": resources,
    }
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(ledger, f)
    return ledger


def print_summary(ledger: dict) -> None:
    s = ledger["summary"]
    print(f"game dir: {s['game_dir']}")
    print(f"exe sha256: {s['exe']['sha256']} match={s['exe']['matches_artifact_of_record']}")
    t = s["totals"]
    print(f"resources: {t['resources']} in {t['containers']} containers, "
          f"{t['resource_bytes']} bytes; by status {t['by_status']}")
    print(f"by source: {t['by_source']}")
    print("gate G1: zero_unhandled =", s["gate_g1"]["zero_unhandled"],
          "unhandled:", s["gate_g1"]["unhandled_type_ids_in_kotor_types"],
          "unknown observed:", s["gate_g1"]["observed_type_ids_not_in_kotor_types"])
    print("cross-checks: all pass =", s["cross_checks_pass"])
    for c in s["cross_checks"]:
        mark = "ok " if c["pass"] else "FAIL"
        print(f"  [{mark}] {c['name']}: expected={c['expected']} actual={c['actual']} ({c['evidence']})")
    print("by type (id label bucket count ok failed open bytes):")
    for tid, bt in s["by_type"].items():
        print(f"  {tid:>5} {bt['label']:<5} {bt['bucket']:<7} n={bt['count']:<7} "
              f"ok={bt['ok']:<7} failed={bt['failed']:<4} open={bt['open']:<4} "
              f"bytes={bt['bytes']}")
    if s["failures"]:
        print(f"failures ({len(s['failures'])}):")
        for r in s["failures"][:20]:
            print(f"  {r['source']}:{r['archive']}[{r['entry']}] {r['resref']} "
                  f"type={r['type']} {r['detail']}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game-dir", required=True,
                    help="KotOR install (KOTOR_GAME_DIR); opened read-only")
    ap.add_argument("--out", default="artifacts/lanes/s1-01-coverage/coverage.json",
                    help="ledger JSON output path (lane artifacts dir)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    game_dir = args.game_dir or os.environ.get("KOTOR_GAME_DIR")
    if not game_dir:
        raise SystemExit("--game-dir or KOTOR_GAME_DIR required")
    log = (lambda *a: None) if args.quiet else (lambda *a: print(*a, file=sys.stderr, flush=True))
    ledger = run(game_dir, args.out, log=log)
    print_summary(ledger)
    print(f"ledger written to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
