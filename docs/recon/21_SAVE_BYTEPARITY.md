# 21 — Save byte parity + VFS P1 closure (lane s1-05)

Status: **byte-parity save writer implemented and proven on the retail
1280×720 quicksave; VFS inferred items closed synthetically or explicitly
handed to s1-06.** All binary claims target build SHA-256
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
(read-only sentinel re-verified this lane: `swkotor.exe` hashes to exactly
that value — **verified**).

New files (this lane only): `tools/resources/kotor_save_parity.py`,
`tools/tests/test_save_parity.py`, `tools/tests/test_vfs_p1.py`, this doc,
`artifacts/lanes/s1-05-save-vfs/`. `kotor_save.py` / `kotor_archive.py` /
`kotor_vfs.py` / `kotor_gff_write.py` are imported read-only, not edited.

## 1. Retail save used

The reference install has **no** retail save (`saves/` contains only
`steam_autocloud.vdf`). The retail save produced by the windowed 1280×720
test exists in the lane-06 profile:
`artifacts/lanes/06-dynamic/profile/saves/000000 - QUICKSAVE/`
(engine-written by the original binary during `elev-save4-noshot-20261024`:
F4 quicksave after loading `000005 - TARIS`, then F5 quickload — see
docs/recon/16 §5). Files: `SAVEGAME.sav` (118,767 B, MOD V1.0, 5 entries:
AVAILNPC0/AVAILNPC2/2027, INVENTORY/**type 0 quirk**/, REPUTE/2038,
tar_m02af/2057), nested `tar_m02af.sav` (3 entries: m02af GIT/2023,
m02af ARE/2012, Module IFO/2014), `savenfo.res`, `PARTYTABLE.res`,
`GLOBALVARS.res` (all GFF V3.2), `Screen.tga`. Validation: **0 errors,
0 warnings** (verified by re-parse this lane).

Caveat (**verified**, from docs/recon/10/16): in this slot `Screen.tga` was
synthesized by lane 06 (256×256 from the HUD capture) because the engine
screenshot resampler `FUN_0045dad0` faults at `0x0045db21` under Mesa; every
*container* (ERF + GFF sidecars) is engine-written. `Screen.tga` is a blob and
is byte-copied by any writer. Secondary retail slots analyzed read-only from
`artifacts/lanes/06-dynamic*/profile/saves/` and `artifacts/quarantine/`
(downloaded vanilla/modded saves): 14 slots total, see
`artifacts/lanes/s1-05-save-vfs/results-all-slots.json`.

## 2. Byte-parity method and results

`kotor_save_parity.py` provides three re-encode modes per container:

* **verbatim** — rebuild from parsed tables + raw slices at original offsets
  (header reconstructed from parsed fields, not copied). Byte-identity proves
  the parser accounts for **every** byte (layout accounting: header /
  localized block / key table / resource table / payload regions / zero gaps /
  trailing for ERF; header + 6 sections + trailing for GFF).
* **repack align=1** — from-scratch ERF writer: entries in order, payloads
  packed contiguously, keys rebuilt from parsed (resref, res_id, type).
* **parity align=1** — from-scratch GFF writer: canonical section order,
  labels first-use, structs pre-order, single-field direct-row rule, fdata
  **without** 4-byte alignment.

Measured on the retail quicksave (`results-byteparity-quicksave.json`):

| scope | count | verbatim | repack/parity from scratch | stock writers |
|---|---:|---:|---:|---:|
| ERF containers (outer + nested) | 2 | **2/2 byte-identical** | **2/2 byte-identical (align=1)** | `ka.erf_bytes` 0/2 |
| GFF payloads (3 sidecars + 3 nested) | 6 | **6/6 byte-identical** | 1/6 (flat `savenfo.res`) | `kotor_gff_write.gff_bytes` 0/6 |
| GFF semantic equality after re-encode | 6 | — | **6/6, 0 check errors** | 6/6, 0 check errors |

Across all 14 real slots: **ERF verbatim 46/46 byte-identical; ERF
repack(align=1) 44/44 byte-identical on every retail (engine-written) ERF**
(the 2 non-identical ones are lane-05's own synthetic `000003 - SYNTH`, which
matches the canonical align-4 writer instead); **GFF verbatim 100%, semantic
equality 100% with 0 check errors everywhere**.

Whole-slot rewrite of the quicksave (`rewrite-parity --mode verbatim`):
**5/5 files byte-identical, nested 1/1, re-parse validation 0 errors / 0
warnings** (`results-rewrite-verbatim.json`). `--mode parity` (from-scratch
encoders): `savenfo.res` + `Screen.tga` byte-identical; `SAVEGAME.sav` differs
only inside the nested module's GFF payloads — first diff at offset 58,702,
inside the `tar_m02af` payload region (outer header/keys/resource table
re-encode identically), validation 0/0 (`results-rewrite-quicksave.json`).

### Diff classification (all verified against the retail bytes)

Retail ERF/MOD/SAV layout rules (measured on the engine-written slot):

1. 160-byte header; `offset_to_localized_string == offset_to_key_list == 160`
   when there are no localized strings; header tail `[56:160)` all zero;
   `build_year=126`, `build_day=296` preserved (engine writes its build date);
   `description_strref=0`.
2. Key records are raw 24-byte: resref latin-1 NUL-padded **preserving case**
   (`AVAILNPC0`, `tar_m02af`), `res_id == entry index`, type u16, unused u16=0.
3. Resource records: 8-byte `(offset, size)`; **payloads are packed
   contiguously with no alignment** (e.g. 320, 8622=320+8302, 20389=…+11767 —
   not even 2-aligned); zero inter-payload gaps; zero trailing bytes.
   `ka.erf_bytes` aligns payloads to 4 and zeroes build year/day — that is the
   entire canonical-writer diff (first diff at offset 32, the
   `offset_to_localized_string` field, +6 B on this file).
4. Quirk: the outer `INVENTORY` entry carries **type 0**, not 2037 — preserved
   by the parity writer (and why `kotor_save.py` matches it by name).

Retail GFF layout rules: sections contiguous from offset 56 in the order
struct/field/label/field_data/field_index/list_index, zero trailing.
`kotor_gff_write.gff_bytes` reproduces struct/field/label **section sizes and
positions exactly** but pads every field-data entry to 4 bytes; retail packs
fdata with **no alignment** (savenfo: fdata 78 B retail vs 85 B canonical) —
that plus shifted downstream offsets is the whole canonical diff on flat
files. For nested GFFs the from-scratch writer additionally differs in the
**global struct/field row interleaving**: retail assigns the root struct's
list-field rows first, then the child structs' rows, then the root's scalar
rows (GLOBALVARS root fidx `[0,1,2,3,1189,…,1192]`), and PARTYTABLE.res
carries **56 orphaned field-index slots (224 B)** — incremental growth copies
of one array left by the engine's append-only writer (root array finally at
slot 397). These two rules are **not derivable from the semantic tree**
(investigated, then documented per the stop rule); consequence: from-scratch
GFF byte-identity is proven for flat files (savenfo) and synthetic fixtures,
while nested retail GFFs are proven **field-level equal** (`gw.semantic`
equality, 0 `gff.check` errors) with the diff classified to struct/field/label
re-ordering + fidx orphans, all size-preserving except PARTYTABLE's +224 B.

Conclusion for the writer contract (**verified**): an unmodified re-encode is
byte-identical (verbatim mode, 100% of files); a from-scratch writer is
byte-identical for ERF containers (retail = contiguous packing, align=1) and
for flat GFFs; for nested GFFs use the verbatim/fixed-point layout or accept
the classified re-ordering — semantic content is identical either way.

## 3. VFS P1 closure (docs/recon/10_SAVE_VFS.md inferred/open items)

Synthetic discriminating fixtures in `tools/tests/test_vfs_p1.py` (12 tests,
all passing) pin each rule; dynamic measurements from
`docs/recon/08_VFS_DYNAMIC.md` (`artifacts/lanes/06-dynamic/vfs-precedence/
runs2/`) are cited where they already exist.

| item (from recon 10/08) | status | evidence |
|---|---|---|
| duplicate-in-archive **KEY first-wins** | **closed** (synthetic + measured 08: byte-patched chitin.key, second `AddKey` returned 0) | `test_key_duplicate_first_record_wins` |
| duplicate-in-archive ERF/RIM first-wins | **closed** (synthetic + measured 08: patch.erf/mainmenu.rim/.mod) | `test_erf_duplicate_first_entry_wins`, `test_rim_duplicate_first_entry_wins` |
| save-nested duplicate: first-wins in the save table; drop happens at conversion output | **closed** (synthetic first-wins; drop site measured 08 `runs2/save-dup-1`) | `test_save_nested_duplicate_first_wins` |
| `.mod` vs RIM: mounted RIM beats `.mod` (pass 3 < 4); loaded-save path mounts `.mod` *instead of* `_s` | **closed** (synthetic resolver rules + measured 08) | `test_rim_beats_mod` |
| save-vs-module: save ERF > `.mod` (pass 4), `< _s` RIM (pass 3); save shadows only stored keys | **closed** (synthetic + measured 08) | `test_save_beats_mod_but_loses_to_s_rim` |
| pass-4 order save > `.mod` > LIPS > players > texture packs | **closed** (synthetic, measured order applied + measured 08) | `test_pass4_measured_order` |
| flat loose dirs, one level, OS enumeration order, pass-1 last-added-first | **closed** (synthetic flat scan + measured-order search + measured 08 `enum-dirs-1`) | `test_flat_one_level_scan_matches_measurement`, `test_pass1_last_added_first` |
| fresh-module area-RIM intra-order (`_a`/`_adx`/`.rim`/`_s`) and `.mod` vs area RIM on New Game | **open → s1-06** (input blocker, not decidable by fixture) | `OPEN_FOR_S1_06` in test_vfs_p1 |
| `streamwaves` audio requests before gameplay | **open → s1-06** (directory/index measured; audio lookups unobserved) | `OPEN_FOR_S1_06` in test_vfs_p1 |

Two resolver defaults in `kotor_vfs.py` contradict the 2026-10-24 measurements
and are pinned as documented gaps (this lane may not edit the tracked file;
**integrator action**): `Vfs._add_dir` defaults to `recursive=True` + name
sort (measured: flat, OS order, no sort), and pass-4 lists texture packs
before lips/players (measured: lips > players > packs). Tests
`test_recursive_default_gap_is_documented` and
`test_pass4_default_order_gap_is_documented` lock the current behavior and
the measured rule side by side.

## 4. Retail acceptance check for lane s1-06 (prepared, NOT run)

Goal: original engine loads a save **rewritten by this lane's parity writer**.
Do not run it in this lane (single game-process lock belongs to s1-06; display
consent rules per SPRINT1). Script + instructions for s1-06:

1. Produce the rewritten slot (already done, or regenerate):
   `python tools/resources/kotor_save_parity.py rewrite-parity "artifacts/lanes/06-dynamic/profile/saves/000000 - QUICKSAVE" "<lane-out>/000000 - QUICKSAVE" --overwrite --mode verbatim --json <lane-out>/rewrite.json`
   (byte-identical by construction — 5/5 files; use `--mode parity` for the
from-scratch variant and diff per file).
2. In a **fresh profile-local working directory** (never the install; verify
   `swkotor.exe` SHA-256 `0290889e…74e6` read-only first — drift = stop),
   copy the rewritten slot in as a new slot, e.g. `Saves/000007 - PARITY/`,
   alongside the untouched originals. Use the windowed-1280×720 patched exe
   and ini from docs/recon/16 §4 (`FullScreen=0`, `Frame Buffer=0`,
   `Shadows=0`, `Soft Shadows=0`, `Grass=0`, `Anti Aliasing=0`).
3. Drive: main menu → Load Game → select `000007 - PARITY` with an immediate
   in-process down/up click (`clickfast` — delayed clicks only hover) →
   expect the loading screen to complete and the HUD at 1280×720
   (`MODULES:tar_m02af_s`, `CURRENTGAME:tar_m02af` in the resman log).
4. Then F5 quickload the slot the engine itself writes on top, and hash-compare
   `gameinprogress/` against the slot. Acceptance = HUD reached + slot
   reloads + per-file SHA-256 of the rewritten slot unchanged after load
   (load must not rewrite the source slot).
5. Record everything under `artifacts/lanes/s1-06-dynamic/` and cite
   `docs/recon/21_SAVE_BYTEPARITY.md`; the thumbnail residual
   (`FUN_0045dad0`@`0x0045db21` under Mesa) still requires the lane-06
   screenshot skip — the save containers are unaffected.

## 5. Tests

`python tools/tests/run_all.py` (with `KOTOR_GAME_DIR` set): **70 tests run,
69 pass, 0 fail, 0 errors, 1 skip** — the skip is the pre-existing
`test_save…test_real_save_if_present` (no save in the reference install; the
retail slot is covered from the lane-06 profile by `test_save_parity.py`).
Lane modules standalone: `test_save_parity.py` **11/11 pass**,
`test_vfs_p1.py` **12/12 pass**. Results:
`artifacts/tests/results/test-results.json`,
`artifacts/lanes/s1-05-save-vfs/results-summary.json` (+ per-slot and
per-file JSONs listed in §1–2).

## 6. Open / blockers

* From-scratch byte-identity for **nested retail GFFs** is not achievable
  without emulating the engine writer's row-interleaving and orphaned-fidx
  growth (documented in §2); verbatim mode covers byte-parity, semantic mode
  covers equality. Marked **open** (low value; engine accepts both layouts —
  the synthetic 97-entry `patch.erf` booted the menu, docs/recon/10).
* Two `kotor_vfs.py` default-vs-measurement gaps flagged for the integrator
  (§3).
* s1-06 items: fresh-module area-RIM intra-order, streamwaves audio
  requests, and the retail acceptance run of the rewritten save (§4).
