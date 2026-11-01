# Recon 18 — Format parser matrix and byte-identical round-trip suite (lane s1-02-formats)

Status: **complete for the shipped corpus of the reference install.**
Target build: SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
(verified read-only at the start of every `validate_all.py` run; exit 2 on drift).

All parsers are stdlib-only, strict, and take bytes (no game data in tests —
tests use synthetic fixtures only). Run everything with:

```
python tools/formats/validate_all.py --game-dir "<KOTOR_INSTALL>"
# -> artifacts/lanes/s1-02-formats/validate_all.json, exit 0 on the reference install
python -m unittest discover -s tools/formats -p "test_*.py"   # 211 tests, all pass
```

Latest full-corpus run (`artifacts/lanes/s1-02-formats/validate_all.json`):
**75,676 / 75,676 payload validations pass, 0 failures; every round-trip class
byte-identical; `all_ok=true`, exit 0, 83.5 s.**

## Per-type matrix

| Type id | Ext | Count (shipped) | Parser module | Validator | Round-trip | Notes |
|---:|---|---:|---|---|---|---|
| 3000 | LYT | 124 | `lyt.py` | counts/sections, 10-token doorhooks, quat WXYZ | parse-only | quirks: duplicate `****` rooms, stray line after `donelayout`, doorhook → unknown room (stunt LYTs); recorded as warnings |
| 3001 | VIS | 112 | `vis.py` | indentation grammar; counts advisory (3/1250 sections disagree) | parse-only | entries may reference sectionless rooms (warning) |
| 2016 | WOK | 1,202 | `wok.py` → `bwm.py` | BWM type 1 + normals; winding stats | parse-only | `position` = header triple @0x3C == MDL node-545 pos (recon 12) |
| 2052/2053 | DWK/PWK | 156+196 | `bwm.py` | BWM type 0 sections | parse-only | 352 BWM total pass |
| 3003 | PTH | 132 | `pth.py` | GFF + `Path_Points`/`Path_Conections` (typo preserved) range checks | parse-only | also hosts `gff_reencode` |
| 2002 | MDL | 3,072 | `mdl.py` | header id 0x413670, name table, node-tree walk (acyclic), anim pair @0x64 | parse-only | node-count mismatch on some models → warning |
| 3008 | MDX | 2,832 | (docs/recon/15; not re-parsed here) | — | — | layout already decoded in recon 15 |
| 3007 | TPC | 11,536 | `tpc.py` | 128-byte header, enc ∈ {1,2,3,4,0xC}, mip chain math, TXI tail | parse-only | data_size semantics vary (mip0 / whole / raw-when-0); cubemaps carry opaque tails — classified, not errors |
| 2022 | TXI | 5,616 | `txi.py` (+writer) | key-value lines, blank lines ok, any non-space key, duplicate keys → warning | parse+write | |
| 3 | TGA | 5,601 | `tga.py` | header + full pixel decode (raw and RLE), TRUEVISION footer, opaque tails allowed | parse-only | |
| — | DDS | 0 shipped | `dds.py` | 124-byte header, fourCC set, exact payload for block formats | synthetic tests only | no DDS in the reference install |
| 2060 | SSF | 316 | `ssf.py` | `SSF V1.1`, lead u32 == 12, u32 StrRefs to EOF | parse-only | sizes 172/208 B observed |
| 3004 | LIP | 18,206 | `lip.py` | `LIP V1.0`, exact 16+5N size, monotonic times, viseme ≤ 15, last == duration | parse-only | inside lips/*.mod |
| 2047 | GUI | 91 | `gui.py` | GFF `GUI V3.2` + recursive control tree (CONTROLTYPE required, CONTROLS optional on leaves) | parse-only | also via `pth.gff_reencode` in GFF round-trip |
| 4 / loose | WAV | 17,191 | `wav.py` | RIFF probe within 512 B (streamsounds prefix = 470 B MP3/LAME frame), chunk tiling, fmt fields; RIFF size may under-declare (streamwaves voice files) | parse-only | PCM + IMA ADPCM (0x11, 4-bit) |
| loose | MP3/BMU | (in stream WAVs) | `mp3.py` | MPEG frame walk (strict for BMU/raw, lenient end for stream prefixes), BMU wrapper (inferred layout) | parse-only | no loose .bmu in this install |
| loose | BIK | 61 | `bik.py` | `BIK`+version, size field == filesize−8, frames pair, dims, frame table (frames+1 u32, ascending, EOF sentinel) | parse-only | all version 'i', 640x272/640x480 |
| 2017 | 2DA | 519 | `two_da.py` (existing) + `two_da_ascii.py` (writers) | V2.b TAB layout (493) and NUL-separated RIM variant (26), V2.0 ASCII | **byte-identical 519/519**; ASCII V2.0 re-parse semantic-equal 519/519 | |
| — | TLK | 1 (49,265 entries) | `tlk.py` (existing) + `tlk_write.py` (writer) | 20-byte header, 40-byte entries, non-overlapping text ranges | **byte-identical**; `tlk_repack` canonical writer for mod appends | |
| — | GFF | 11,143 | `gff.py` (existing) + `pth.gff_reencode` | check + check_values | **byte-identical 11,143/11,143** | layout-preserving re-encode (raw arenas kept) |
| — | ERF/MOD | 128 | `kotor_archive.py` (existing) + `validate_all.erf_reencode_check` | 160-byte header, 24-byte keys, gapless payloads | **byte-identical 5/5** (patch.erf + 4 TexturePacks) | lips/*.mod parse-only |
| — | RIM | 246 | `kotor_archive.py` + `validate_all.rim_reencode_check` | 20-byte header, 32-byte entries | **byte-identical 246/246** (all 234 module + 12 global RIMs; payload offsets preserved — shipped RIMs carry 8–19 B writer gaps) | |
| — | BIF | 26 | `kotor_archive.py` + `validate_all.bif_reencode_check` | 20-byte header, 16-byte records, gapless payloads | **byte-identical 26/26** | |
| — | KEY | 1 | `kotor_archive.py` + `validate_all.key_reencode_check` | 64-byte header (build year/day at 0x18/0x1C preserved), 12-byte file records, 22-byte keys | **byte-identical** (25,836 keys, 26 BIFs) | |

## Round-trip classes (task items a–d)

| class | scope | result |
|---|---|---|
| (a) GFF writer re-encode | all 11,143 GFF payloads in BIFs/RIMs/patch.erf | 11,143/11,143 **byte-identical** (`pth.gff_reencode`) |
| (b) 2DA writer re-encode | all 519 shipped tables (209 in 2da.bif + 306 in global RIMs + 4 in patch.erf) | 519/519 **byte-identical** V2.b re-encode; 519/519 ASCII V2.0 parse+write semantic-equal |
| (c) TLK writer re-encode | dialog.tlk, 49,265 entries | **byte-identical** (text offset + blob preserved); repack re-parses to identical entries/texts |
| (d) ERF/MOD + RIM + BIF/KEY re-encode | patch.erf + 4 TexturePacks ERFs; all 234 module RIMs + 12 global RIMs; all 26 data/*.bif + chitin.key | **byte-identical: ERF 5/5, RIM 246/246, BIF 26/26, KEY 1/1** |

No class required the semantic-equality fallback: every shipped class is
byte-identical. Diff classifications that were investigated and resolved
during the run (all recorded per-file in the JSON): RIM payload placement gaps
(8–19 B, preserved verbatim — a gapless repack would differ, classified as
padding), the 26 NUL-separated V2.b RIM tables (own writer mode), KEY header
build year/day, ERF gapless payload layout.

## New tools (integrator: add to tools/README.md)

`tools/formats/`: `lyt.py`, `vis.py`, `bwm.py`, `wok.py`, `pth.py` (also
`load_gff_bytes` + `gff_reencode`), `mdl.py`, `tpc.py`, `txi.py`, `dds.py`,
`tga.py`, `ssf.py`, `lip.py`, `gui.py`, `bik.py`, `wav.py`, `mp3.py`,
`two_da_ascii.py` (2DA writers incl. NUL variant + ASCII), `tlk_write.py`
(TLK writer — new file because `tlk.py` is tracked and read-only for this
lane), `validate_all.py`, and `test_<each>.py` suites (211 tests total
including the pre-existing `test_gff_types.py`/`test_two_da.py`).

## Verified vs inferred vs open

- **Verified** (corpus counts above, all cited in `validate_all.json`):
  every layout claim in the module docstrings; BWM 136-byte header with 16
  section fields and section chaining; WOK position triple @0x3C; LYT/VIS
  grammars; TPC header/chain/TXI-tail rules; SSF lead u32 == 12; LIP frame
  packing; BIK 'i' header + frame table with EOF sentinel; WAV prefix and
  under-declared RIFF sizes; MP3 frame walk on stream data; ERF/RIM/BIF/KEY
  re-encode byte-identity.
- **Inferred**: BMU wrapper layout (`BMU ` + u32 size + MP3) — no shipped
  .bmu in this install; RIM payload gap rule (offsets preserved, not
  re-derived); TPC data_size exact semantics for cubemaps/odd GUI textures
  (opaque tails accepted).
- **Open**: none blocking. DDS has no shipped instances (validator is
  synthetic-fixture-tested only). MDX payload parsing is intentionally left
  to `docs/recon/15` (already fully decoded there).

## Reproduce

```
python tools/formats/validate_all.py --game-dir "E:/SteamLibrary/steamapps/common/swkotor"
python -m unittest discover -s tools/formats -p "test_*.py"
```

Raw probe dumps used while deriving the layouts are under
`artifacts/lanes/s1-02-formats/probes/` (gitignored).
