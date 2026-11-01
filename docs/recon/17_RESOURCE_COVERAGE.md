# Recon 17 — G1 resource coverage ledger (lane s1-01-coverage)

Date: 2026-11-01. Target: user-owned KotOR 1.0.3.0 install, executable SHA-256
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6` (verified at run start;
run aborts on drift). All access read-only; no game bytes copied into tracked files.

Note: `docs/SPRINT1.md` does not exist at this branch head (only `SPRINT0.md` /
`SPRINT0_REPORT.md`); this lane followed the s1-01-coverage assignment text and the
Sprint 0 shared-resource rules. **Open** for the integrator: add `SPRINT1.md`.

Tooling: `tools/inventory/resource_coverage.py` (enumerators + header validators +
ledger + gate check), tests `tools/inventory/test_resource_coverage.py`. Reuses
`tools/resources/kotor_archive.py` (KEY/BIF/RIM/ERF readers), `tools/inventory/kotor_types.py`
(type table), and the real parsers `tools/formats/gff.py`, `tools/formats/two_da.py`,
`tools/formats/tlk.py`.

Artifacts (gitignored, metadata only): `artifacts/lanes/s1-01-coverage/coverage.json`
(full ledger, 93,053 resource records), `run_stdout.txt` (summary), `run_stderr.txt`
(progress), `gametest.txt` + `coverage_gametest.json` (opt-in unittest run).

## 1. Method

Every resource is enumerated from its container, payload-read, SHA-256-hashed, and
assigned a parser bucket + status (`ok` / `failed` / `open`):

| source | enumeration | resources | bytes |
|---|---|---:|---:|
| `chitin.key` → `data/*.bif` (26 BIFs) | KEY entries resolved to BIF records by table index | 25,836 | 1,315,698,184 |
| `modules/*.rim` (234 RIMs) | RIM entry tables | 20,575 | 71,421,011 |
| `rims/*.rim` (12 global RIMs) | RIM entry tables | 1,936 | 38,914,383 |
| `patch.erf` | ERF (KotOR layout) | 97 | 7,240,279 |
| `TexturePacks/*.erf` (4 ERFs) | ERF | 11,453 | 672,040,451 |
| `lips/*.mod` (123 MODs = ERF) | ERF entries, all type 3004 LIP | 18,206 | 5,023,596 |
| loose: `dialog.tlk`, `Override/`, `streamwaves/`, `streamsounds/`, `streammusic/`, `movies/` | file walk (regular files only; `streamwaves/` contains subdirectories) | 14,950 | 1,476,740,101 |
| **total** | 402 containers | **93,053** | **3,587,078,005** |

Record schema per resource: `source, archive, entry, resref, type, type_label, size,
sha256, magic (first 16 bytes hex), bucket, status, detail`.

## 2. Gate G1 — zero unhandled types (verified)

Every type id in `kotor_types.py` maps to a bucket (`TYPE_BUCKET`), and every bucket
has a validator (`VALIDATORS`); the test `test_every_kotor_type_has_a_bucket` asserts
this. Observed type ids in the install are all in `kotor_types.py`.
`summary.gate_g1.zero_unhandled = true`, `unhandled = []`, `observed unknown = []`.

Buckets and their validators (real parsers first, header validators stdlib-only):

| bucket | types | validator | evidence |
|---|---|---|---|
| gff | ARE/IFO/BIC/GIT/UTI/UTC/DLG/ITP/UTT/UTS/FAC/UTE/UTD/UTP/GUI/UTM/JRL/UTW/PTH/BTI/BTC/GFF | `tools/formats/gff.py` `Gff` + `check()`; magic must equal the type label, version `V3.2` | recon 01 §4, 07_GFF_CORPUS; observed `ARE V3.2`, `BTC V3.2` |
| two_da | 2DA | `tools/formats/two_da.py` `parse_2da`; fallback header validation for the NUL-separated V2.b variant (see §5) | recon 02, 05_2DA |
| tlk | TLK | `tools/formats/tlk.py` wire layout (header + entry/string table bounds) | recon 01 §3 |
| ncs | NCS | header: `NCS V1.0` + `0x42` + u32be size == file size (13-byte BE header) | recon 09; observed `4e43532056312e30 42 00000017` for a 23-byte script |
| text/txt | NSS, TXT | printable-ratio check | recon 01 |
| txi | TXI | printable key/value lines (numeric first line allowed, observed `filmnoisetex`) | recon 01 |
| lyt | LYT | `#MAXLAYOUT ASCII` + section state machine (`<x>count N` + exactly N entry lines; rooms/tracks/obstacles = name+3 floats, doorhooks = name+door+8 numbers) | observed m01aa/m45mg/mgf_ebonhawk |
| vis | VIS | printable room-pair lines | recon 01 |
| ssf | SSF | `SSF V1.1` + u32-aligned StrRef table ≥ 28×4 bytes | recon 02 §Data formats |
| lip | LIP | `LIP V1.0` + f32 duration + u32 frames + `16+5·frames == size`, viseme ≤ 15 | recon 02; observed 206 = 16+38·5 |
| bwm | WOK/DWK/PWK | `BWM V1.0` + type@8 + 16 u32 counts/offsets@72; vertex/face/normal blocks in range | recon 01/02; tools/geometry/probe.py |
| mdl | MDL | first u32 == 0; 12-byte header (offsets exclude it); root@52 and mdx block@188/192 in range | recon 02, 15 |
| mdx | MDX | no header: u32-aligned block stream starting at byte 0 (stride comes from the paired MDL) | recon 15 (payload+sentinel+pad == file size, exact) |
| tga | TGA | 18-byte header sanity (dtype, dims, bpp) + lower-bound size for uncompressed types | recon 02; shipped `chrome1.tga` carries 26 trailing bytes, so no exact-size rule |
| wav | WAV | `RIFF…WAVE` probed within first 1 KiB (some SFX carry a ~470-byte prefix) + `fmt ` chunk + RIFF size in range | recon 02 §Data formats |
| mp3 | BMU | `BMU` prefix / `ID3` / MPEG frame sync | recon 02 |
| bik | BIK | `BIK` + version letter (observed `BIKi`) | recon 02; movies/01a.bik |
| dds | DDS | `DDS ` + header size 124 + ≥128 bytes | Kaitai spec (MIT) |
| tpc | TPC | 128-byte header: dims@0x08/0x0A in 1..8192, encoding@0x0C ∈ {0,1,2,4,0xC}, mips@0x0D ≤ 16, dataSize@0 fits, alpha-test float@4 in [0,1]; TXI appended after mip data | recon 02; observed patch.erf `1280x1024back` |
| ini | INI | text + ≥1 `[section]` + `key=value` lines | — |
| ltr | LTR | `LTR V1.0` + u32 count | recon 01 (resref `humanf`) |
| erf/rim/bif/key | MOD/SAV/ERF/RIM/BIF/KEY payloads | `tools/resources/kotor_archive.py` readers | recon 01 |
| probe/reserved | fallback | magic recorded, status `open` (none shipped after BTI/BTC reclassification) | — |

## 3. Results (verified against the install, read-only run 2026-11-01)

Totals: **93,053 resources in 402 containers, 3,587,078,005 payload bytes;
93,053 ok / 0 failed / 0 open.** Executable hash matched the artifact of record at
run start (`match=True`).

Per-type table (count, status, bytes) — full detail in `coverage.json`:

| type | label | bucket | count | ok | failed | open | bytes |
|---:|---|---|---:|---:|---:|---:|---:|
| 3 | TGA | tga | 5,601 | 5,601 | 0 | 0 | 114,462,157 |
| 4 | WAV | wav | 17,191 | 17,191 | 0 | 0 | 1,076,575,921 |
| 2002 | MDL | mdl | 3,072 | 3,072 | 0 | 0 | 494,604,005 |
| 2009 | NSS | text | 1,774 | 1,774 | 0 | 0 | 1,784,742 |
| 2010 | NCS | ncs | 13,139 | 13,139 | 0 | 0 | 35,869,932 |
| 2012 | ARE | gff | 117 | 117 | 0 | 0 | 463,804 |
| 2014 | IFO | gff | 117 | 117 | 0 | 0 | 193,767 |
| 2015 | BIC | gff | 1 | 1 | 0 | 0 | 4,815 |
| 2016 | WOK | bwm | 1,202 | 1,202 | 0 | 0 | 25,715,912 |
| 2017 | 2DA | two_da | 519 | 519 | 0 | 0 | 1,160,000 |
| 2018 | TLK | tlk | 1 | 1 | 0 | 0 | 5,394,446 |
| 2022 | TXI | txi | 5,616 | 5,616 | 0 | 0 | 333,478 |
| 2023 | GIT | gff | 117 | 117 | 0 | 0 | 2,996,409 |
| 2024 | BTI | gff | 1 | 1 | 0 | 0 | 514 |
| 2025 | UTI | gff | 1,055 | 1,055 | 0 | 0 | 1,106,840 |
| 2026 | BTC | gff | 3 | 3 | 0 | 0 | 8,656 |
| 2027 | UTC | gff | 1,958 | 1,958 | 0 | 0 | 6,389,952 |
| 2029 | DLG | gff | 1,167 | 1,167 | 0 | 0 | 24,743,931 |
| 2030 | ITP | gff | 16 | 16 | 0 | 0 | 109,582 |
| 2032 | UTT | gff | 1,121 | 1,121 | 0 | 0 | 1,133,322 |
| 2035 | UTS | gff | 635 | 635 | 0 | 0 | 687,907 |
| 2036 | LTR | ltr | 3 | 3 | 0 | 0 | 819,531 |
| 2038 | FAC | gff | 20 | 20 | 0 | 0 | 32,084 |
| 2040 | UTE | gff | 133 | 133 | 0 | 0 | 148,690 |
| 2042 | UTD | gff | 575 | 575 | 0 | 0 | 1,033,487 |
| 2044 | UTP | gff | 1,784 | 1,784 | 0 | 0 | 3,549,219 |
| 2047 | GUI | gff | 91 | 91 | 0 | 0 | 1,817,175 |
| 2051 | UTM | gff | 38 | 38 | 0 | 0 | 127,759 |
| 2052 | DWK | bwm | 156 | 156 | 0 | 0 | 63,776 |
| 2053 | PWK | bwm | 196 | 196 | 0 | 0 | 63,040 |
| 2056 | JRL | gff | 4 | 4 | 0 | 0 | 78,703 |
| 2058 | UTW | gff | 2,058 | 2,058 | 0 | 0 | 1,540,729 |
| 2060 | SSF | ssf | 316 | 316 | 0 | 0 | 55,216 |
| 2063 | BIK | bik | 61 | 61 | 0 | 0 | 606,082,480 |
| 3000 | LYT | lyt | 124 | 124 | 0 | 0 | 139,911 |
| 3001 | VIS | vis | 112 | 112 | 0 | 0 | 131,449 |
| 3003 | PTH | gff | 132 | 132 | 0 | 0 | 1,010,872 |
| 3004 | LIP | lip | 18,206 | 18,206 | 0 | 0 | 5,023,596 |
| 3007 | TPC | tpc | 11,536 | 11,536 | 0 | 0 | 679,007,288 |
| 3008 | MDX | mdx | 3,085 | 3,085 | 0 | 0 | 492,612,908 |

GFF bucket total: **11,143** — exactly matching lane 03's corpus
(`docs/recon/07_GFF_CORPUS.md`: BIF 1,362 + module RIM 9,712 + global RIM 62 +
patch ERF 7 = 11,143, including BTI 1 + BTC 3). Cross-source agreement: verified.

### Cross-checks against recon 01 / recon 06 (all pass)

All 18 checks in `summary.cross_checks` pass (`cross_checks_pass = true`): 26 BIF
files / 25,836 KEY resources / 234 module RIMs / 20,575 module resources /
71,421,011 module entry bytes / 12 global RIMs / 1,936 global resources /
38,914,383 global entry bytes / 97 patch entries / 4 texture ERFs / 11,453
texture entries / 1 dialog.tlk / 13,799 streamwaves / 970 streamsounds /
119 streammusic / 61 movies / 123 lips MODs / 0 Override files — each cited to
`docs/recon/01_ARTIFACTS.md` §2–3 or `docs/recon/06_MANIFEST.md`.

## 4. Findings worth recording

- **BTI (2024) / BTC (2026) are GFF V3.2**, magic `BTI `/`BTC ` (payload magic in the
  ledger; lane 03's corpus already included them). This closes the "BTI/BTC binary
  layouts" part of recon 01 open question 3. Verified.
- **26 shipped 2DAs use a NUL-separated V2.b variant** (column names and row labels
  NUL-separated, column blob ends with a double NUL) in `rims/global.rim` and
  `rims/miniglobal.rim`; the base `data/2da.bif` copies of the same tables are
  TAB-separated. The strict parser (`tools/formats/two_da.py`) rejects the NUL
  variant ("last column name is not TAB-terminated"); this lane's ledger validates
  them with a structural header fallback (magic + all table ranges). Verified on
  all 26. Open for lane 02/integrator: teach `two_da.py` the NUL variant.
- **Texture packs A/B/C carry identical resref sets** (3,294 each, sets equal) —
  verified. The quality-selection rule remains open (recon 08). Open.
- **8,290 duplicate (resref, type) pairs across archives** (max 117 copies, e.g. 2DA
  tables repeated in `2da.bif`, `global.rim`, `miniglobal.rim` and module RIMs) —
  matters for VFS precedence (recon 08). Verified.
- MDL/MDX resref pairing: 2,890 MDL resrefs, all paired with an MDX; 11 MDX-only
  resrefs. Verified.
- `streamwaves/` contains subdirectories (e.g. `globe/`) — the loose walk enumerates
  regular files only; file counts still match 06_MANIFEST exactly. Verified.
- Extensionless loose files in `streamwaves/` are sniffed by RIFF probe → WAV.
  Verified.

## 5. Gaps triaged as open

| item | status | reason |
|---|---|---|
| BTI/BTC semantics | closed (verified) | GFF V3.2 payloads; see §4 |
| 26 NUL-variant 2DAs | handled (header-validated), parser update open | strict `two_da.py` rejects the NUL-separated variant; ledger fallback passes all 26 |
| MDX block-level decode in the ledger | open by design | MDX has no file header; block strides come from the paired MDL (recon 15). Ledger checks u32 alignment; full decode is lane 10's `mdx2` |
| TGA exact size | open by design | shipped TGA carries trailing bytes (observed +26 on `chrome1.tga`); only header + lower bound enforced |
| texture-pack quality mapping (tpa/tpb/tpc) | open | selection rule not measurable read-only; recon 08 |
| WAV ~470-byte prefix frequency | open | validator tolerates it; exact prefix layout not decoded here |
| `docs/SPRINT1.md` missing | open | integrator to add the Sprint 1 doc |

## 6. Tests

- Synthetic suite (no game data): `python -m unittest -v test_resource_coverage`
  from `tools/inventory/` — **25 tests, 25 pass, 1 skipped** (the skip is the
  opt-in install test). Covers every validator (good + malformed payloads), the
  gate invariant (every `kotor_types.py` id has a bucket + validator), the
  end-to-end fixture install (KEY/BIF + module RIM + global RIM + patch ERF +
  texture ERF + lips MOD + loose dirs), cross-check pass, record schema, container
  hashing, and the install-drift abort.
- Read-only install run: `python tools/inventory/resource_coverage.py
  --game-dir "$KOTOR_GAME_DIR" --out artifacts/lanes/s1-01-coverage/coverage.json`
  — 93,053/93,053 resources ok, 0 failed, 0 open, gate G1 zero unhandled, 18/18
  cross-checks pass, exe hash matched. Also exercised via
  `KOTOR_GAME_DIR=… KOTOR_COVERAGE_GAME_TEST=1 python -m unittest
  test_resource_coverage.GameDirRunTests` (see `gametest.txt`).

## 7. Reproduce

```bash
cd <repo>
python -m unittest -v tools/inventory/test_resource_coverage.py   # synthetic
python tools/inventory/resource_coverage.py --game-dir "$KOTOR_GAME_DIR" \
    --out artifacts/lanes/s1-01-coverage/coverage.json            # read-only install
```

## 8. Evidence status

- Verified: all counts and statuses above (ledger + cited recon numbers); exe hash;
  BTI/BTC = GFF; NUL-variant 2DAs; texture-pack resref equality; duplicate pairs.
- Inferred: bucket assignments for types absent from this install (DDS/MP3/INI/TXT/
  MOD/SAV/ERF/RIM/BIF/KEY payloads) use the spec-based validators; none occur as
  shipped payloads except through loose-file sniffing (WAV/BIK).
- Open: items in §5.
