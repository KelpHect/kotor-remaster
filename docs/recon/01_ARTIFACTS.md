# Recon 01 — Artifacts, provenance, content inventory

Date: 2026-10-23. Target: Star Wars: Knights of the Old Republic (2003), English, user-owned install.
Everything here is observation on the user's local installation. No game content is committed to the repo.

## 1. Artifact of record

| Field | Value |
|---|---|
| Path | `<KOTOR_INSTALL>\swkotor.exe` (local user-supplied install) |
| Product | KotOR 1.0.3.0, BioWare Corp. |
| Size | 4,042,752 bytes |
| SHA-256 | `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6` |
| MD5 | `a9f8b94a9423dfa4de8c9b44ff57d932` |
| mtime (UTC) | 2026-10-22T23:52:58Z |
| Format | PE32, x86, image base `0x400000`, entry RVA `0x2fb38d` (VA `0x6fb38d`) |
| Imports | OPENGL32, GLU32, DINPUT8, MSS32 (Miles), BINKW32, KERNEL32, USER32, GDI32, ole32, VERSION, IMM32 |
| Sections | `.text` va `0x1000` vsize `0x33b1d0`; `.rdata` va `0x33d000`; `.data` va `0x38d000`; `.rsrc` va `0x436000` |
| Wrapper | No `.bind` section; entry point in `.text`. This is the user-owned 1.0.3.0 build used for the program. |

Compiler fingerprint still open: no Rich header was found; the build predates modern MSVC metadata.
No PDB. Static analysis of this build is the basis of the program. No DRM circumvention is performed.

### Stale analysis target (discarded)

An earlier Ghidra program reported SHA-256 `34e6d971c034222a417995d8e1e8fdd9f8781795c9c289bd86c499a439f34c88`
with a `.bind` section and entry `0x86d2ed` — a different (wrapped) build. It does not match the file above
and must not be cited. A fresh headless project was created from the current file:

- Project: `artifacts/ghidra/kotor.gpr` (gitignored)
- Import log: `artifacts/ghidra/analyze.log`; loader MD5 `a9f8b94a9423dfa4de8c9b44ff57d932` matches.
- Ghidra 12.1.4 on JDK 25. Ghidra 12.1.4 validates JDK 21; pin JDK 21 for reproducible runs.
- Analysis was still running when this note was written; function counts are pending.

## 2. Installation inventory

Total 3.4 GB.

| Path | Files | Size | Notes |
|---|---:|---:|---|
| `data/` | 26 | 1.3 GB | 26 BIF archives |
| `modules/` | 234 | 70 MB | RIM; 117 area pairs (`X.rim` + `X_s.rim`) |
| `rims/` | 12 | 38 MB | global/menu RIMs |
| `lips/` | 123 | 5.7 MB | LIP files |
| `movies/` | 61 | 579 MB | Bink video |
| `streammusic/` | 119 | 191 MB | streamed music |
| `streamsounds/` | 970 | 115 MB | streamed SFX |
| `streamwaves/` | 13,799 | 553 MB | streamed voice/WAV |
| `TexturePacks/` | 4 | 642 MB | loose texture packs |
| `Override/` | 0 | 0 | empty |
| `chitin.key` | — | 569,239 B | resource index |
| `dialog.tlk` | — | 5,394,446 B | string table |
| `patch.erf` | — | 7,243,543 B | 97 entries: 84 TPC, 7 GUI, 4 2DA, 1 MDL, 1 MDX |
| `swkotor.exe` | — | 4,042,752 B | artifact of record |

## 3. Archive parsing results (verified)

### chitin.key

- 25,836 base resources, 26 BIF archives, 22-byte key records.
- Header is `KEY V1  `; community docs call the KotOR variant V1.1. The 12-byte file records are
  observed as `{size u32, nameOffset u32, nameLength u16, drives u16}` (all drives values are 1).
  Reading nameLength as u32 yields 65,549 and is invalid for this build, so do not follow that note.
  Key records are 22 bytes; the final 4-byte field carries the BIF index in bits 20–31.
- `ResId` encoding observed: **BIF index = `resid >> 20`, table index = `resid & 0xFFFFF`**.
  (The earlier `resid >> 32` interpretation was wrong; a 32-bit field cannot carry the BIF index above bit 31.)
- 2DA keys occupy `resid` 0..208, consistent with BIF 0 = `2da.bif`.
- MDL/MDX keys span BIF indices 2, 18, 19, 20 = `items.bif`, `models.bif`, `party.bif`, `player.bif`.

### BIFF archives

- Signature `BIFF V1  `, 20-byte header: `varCount`, `fixedCount`, `offsetToVarTable`.
- Resource records: 16 bytes `{id, offset, size, type}`; variable table at `offsetToVarTable`,
  fixed table immediately after `varCount * 16`. Resource payloads are uncompressed.
- Key-to-BIF reconciliation is exact for all 26 archives (e.g. `models.bif`: 6,444 keys / 6,444 records;
  `2da.bif`: 209 / 209).

### RIM modules

- Signature `RIM V1.0`, 20-byte header (reserved u32, count at +12, entry-table offset at +16).
- Entries are 32 bytes: `{resRef[16], type, id, offset, size}`.
- 234 RIM files, **20,575 resources**, **71,421,011 bytes** of entry payload.
- 117 each of ARE/GIT/IFO confirms 117 distinct module identities (plus menu/global RIMs elsewhere).
- `rims/`: 12 global/menu RIMs, **1,936 resources**, **38,914,383 bytes**:
  WAV 375, NCS 492, 2DA 306, MDL 239, MDX 252, SSF 210, UTI 62.

### ERF archives (patch and texture packs)

ERF header in this build places `entryCount` at `0x10`, key table at `0x14`, resource table at `0x1C`;
keys are 24 bytes, resource records 8 bytes. The parser validates both this layout and the common
NWN layout and records which one succeeded.

- `patch.erf`: **97 entries** — 84 TPC, 7 GUI, 4 2DA, 1 MDL, 1 MDX. It is the 1280×1024 UI/texture
  patch (resrefs like `1280x1024back`, `keymap`, `inventory`, `PFHA01*`) and overrides base UI data.
- `TexturePacks/swpc_tex_gui.erf`: 1,571 entries — 1,570 TPC + 1 TXI.
- `TexturePacks/swpc_tex_tpa.erf`, `tpb`, `tpc`: 3,294 TPC each, with identical resrefs across the
  three files. They are alternate texture-quality sets (A/B/C); the selection rule is an open question.
- Total texture-pack content: **11,452 TPC** (642 MB on disk) before dedup by resref.

### TLK

- `dialog.tlk`: `TLK V3.0`, **49,265 string entries**, 40-byte entries, text blob at `0x1E11BC`.

### Aggregate

Approximate pre-dedup resource count: 25,836 (base) + 20,575 (modules) + 1,936 (global RIMs) +
97 (patch) + 11,453 (texture packs) ≈ **59,897**, excluding loose waves, movies, LIP files, and saves.

## 4. Resource type table (observed payload signatures)

`signature` = magic read from the payload; `inferred` = role inferred from context and not yet parser-confirmed.

| ID | Ext | Confidence | Evidence |
|---:|---|---|---|
| 3 | TGA | signature + spec | binary Targa image; lightmaps paired with TXI. (TPC is type 3007; not present in base BIFs) |
| 4 | WAV | signature | `RIFF....WAVEfmt` |
| 2002 | MDL | signature | binary model |
| 2009 | NSS | signature | `//forward declares...` source text |
| 2010 | NCS | signature | `NCS V1.0` bytecode |
| 2012 | ARE | signature | `ARE V3.28` |
| 2014 | IFO | signature | `IFO V3.28` |
| 2015 | BIC | signature | `BIC V3.28` |
| 2016 | WOK | inferred + spec | `BWM V1.0` area walkmesh |
| 2017 | 2DA | signature | `2DA V2.b` |
| 2022 | TXI | signature | `islightmap 1...compresste...` |
| 2023 | GIT | signature | `GIT V3.28` |
| 2024 | BTI | inferred | binary template, 1 sample |
| 2025 | UTI | signature | `UTI V3.28` |
| 2026 | BTC | inferred | binary template, 3 samples |
| 2027 | UTC | signature | `UTC V3.28` |
| 2029 | DLG | signature | `DLG V3.28` |
| 2030 | ITP | signature | `ITP V3.28` |
| 2032 | UTT | signature | `UTT V3.28` |
| 2035 | UTS | signature | `UTS V3.28` |
| 2036 | LTR | inferred | `LTR V1.0`, resref `humanf` |
| 2038 | FAC | signature | `FAC V3.28` |
| 2040 | UTE | signature | `UTE V3.28` |
| 2042 | UTD | signature | `UTD V3.28` |
| 2044 | UTP | signature | `UTP V3.28` |
| 2047 | GUI | signature | `GUI V3.28` |
| 2051 | UTM | signature | `UTM V3.28` |
| 2052 | DWK | inferred + spec | `BWM V1.0`; seen on door models |
| 2053 | PWK | inferred + spec | `BWM V1.0`; seen on placeable models |
| 2056 | JRL | signature | `JRL V3.28` |
| 2058 | UTW | signature | `UTW V3.28` |
| 2060 | SSF | signature | `SSF V1.1` |
| 3000 | LYT | signature | `#MAXLAYOUT ASCII` |
| 3001 | VIS | signature | room visibility list |
| 3003 | PTH | signature | `PTH V3.28` |
| 3008 | MDX | signature | binary vertex data |

## 5. Content magnitudes relevant to scope

Base archives (25,836):

- NCS 1,784; NSS 1,774; MDL 2,832; MDX 2,832; TGA 5,601; TXI 5,615; WAV 1,928; 2DA 209; GUI 84;
  WOK 1,202 / DWK 156 / PWK 196; UTC 205; UTI 557; UTP 317; UTM 106; UTE 65; UTD 50; UTT 21;
  UTW 9; SSF 16; LYT 124; VIS 112; DLG 32; JRL 1; BIC 1; BTI 1; BTC 3; ITP 16; LTR 3; FAC 0.
- TPC (type 3007) is absent from the base BIFs; the 642 MB `TexturePacks/` and loose files
  must be enumerated separately. The 5,601 type-3 resources are TGA (lightmaps/textures).

Module RIMs (20,575):

- NCS 10,863; UTC 1,753; UTW 2,049; UTP 1,467; UTI 436; DLG 1,135; UTT 1,100; UTS 635; UTD 525;
  UTE 68; UTM 38; FAC 20; PTH 132; ARE/GIT/IFO 117 each; JRL 3.

Combined (before dedup): ~46,411 resources, including **12,647 NCS**, **1,958 UTC**, **1,784 UTP**,
**1,167 DLGs**, **993 UTIs**, **2,058 UTWs**, and **1,554 walkmeshes** (1,202 WOK + 156 DWK + 196 PWK).

### Binary inventory (fresh Ghidra project)

- Analysis succeeded on the artifact of record. Metadata export: **8,616 functions**,
  **809,297 instructions**, **2,711,359 bytes** of function bodies, 60 thunks, 311 non-`FUN_` names.
- Largest exported function: `0x5fcd10`, 21,905 bytes / 5,302 instructions (unclassified).
- Imported stubs are represented as library-namespaced functions/thunks and still need classification;
  the metadata field `external` was false for all rows, so do not treat it as an import count.
- Full pseudocode pass complete for the initial function inventory: **8,616/8,616 functions
  decompiled, 0 errors**, 13,513,607 characters of C pseudocode
  (`artifacts/ghidra/functions-full.jsonl`). The project is `artifacts/ghidra/kotor.gpr`.
- Action table fully mapped: `0x0054c960` allocates `0xC10` (772 × 4), zeroes it, writes 677 slots,
  then tail-jumps to `0x005cd2c0`, which writes 97 more. Total **774 writes / 772 unique slots**
  (id 156 `EffectTemporaryForcePoints` is written twice; the later `0x534340` wins; id 709
  `GetNPCSelectability` is written twice with the same handler). Names come from the shipped
  `nwscript.nss` (772 declarations).
- The earlier 757/772 count was an extraction artifact: Ghidra printed 15 store offsets in decimal
  and slot 0 without an offset, so a hex-only regex missed them. The corrected extractor reproduces
  772/772.
- Dispatcher verified at **`0x0052c0d0`** (missing from the initial Ghidra pass; defined in the
  coverage copy): it checks id < 772, loads `[ecx+0xC][id]`, rejects null/out-of-range with `-2002`
  (`0xFFFFF82E`), and calls the handler with `(id, arg)`. Confirmed from the bytes at VA
  `0x0052c0d0`. The service vtable `0x7450e4` contains `0x0054c960` at +4 and `0x0052c0d0` at +8;
  both pointers are present in the file.
- Coverage pass on an isolated project copy: all **440 `LAB_` handler targets plus the dispatcher**
  were defined as functions and decompiled (**441/441, 0 failures**). The copy contains **9,057
  internal functions** (8,616 baseline + 441) and **350 external imports**, for 9,407 total
  `getFunctionCount()`; no baseline function was lost. A follow-up pass renamed all 125 pre-existing
  `FUN_` handler targets, so 565/565 unique targets are named and all 772 actions map; the
  dispatcher is intact. Runtime dispatch behavior is not yet executed. Artifacts:
  `artifacts/analysis/ghidra-handlers/{coverage.json,verification-independent.json,new_functions.jsonl}`,
  `artifacts/action_table.json`, `artifacts/game-extract/nwscript.nss` (local only).

Important: module NCS counts include the same script names across modules; content hashing is required
for a unique-script count. Source is shipped for 1,774 scripts; the rest must be decompiled or
behaviorally reconstructed.

## 6. Reproduce

```bash
python tools/inventory/kotor_inventory.py \
  --game-dir "<KOTOR_INSTALL>" \
  --out artifacts/inventory.json

python tools/inventory/resource_probe.py \
  --game-dir "<KOTOR_INSTALL>" --per-type 1

python tools/inventory/pe_summary.py \
  "<KOTOR_INSTALL>/swkotor.exe"
```

## 7. Open questions

1. ERF header field order (97-entry patch parsed successfully) and the exact VFS precedence
   (five-pass model documented; still verify dynamically), including the A/B/C texture selection.
2. Texture-pack quality-to-archive mapping and the first-launch loose-file/Override quirk.
3. BWM section semantics (WOK/DWK/PWK types mapped), BTI/BTC binary layouts, type 2036 semantics.
4. Unique NCS count and NSS↔NCS pairing after content hashing.
5. Compiler fingerprint (no Rich header) and C++ runtime signatures.
6. Runtime coverage: all 565 handler targets are now named (125 `FUN_` renames + 440 `LAB_`), but
   dispatch behavior and handler effects have not been executed.
7. Final pseudocode coverage for the coverage copy (441 handler/dispatcher functions added to the
   initial 8,616/8,616 baseline; all 441 decompiled).
8. Dynamic-analysis harness integration and deterministic replay tolerances.
9. Localization: which TLK/voice assets exist for this install; language ID.
