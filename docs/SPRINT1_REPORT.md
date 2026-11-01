# Sprint 1 exit report — data completeness and map-compile freeze (P1, gate G1)

Status: **complete — gate G1 met.** Eight lanes ran against the user-owned KotOR 1.0.3.0 install
(SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`); the install was only
ever read (sentinel drift 0 at every preflight/final check). The dynamic lane ran under explicit
session display consent (focused main-screen, windowed 1280×720 patched **copy** in a lane profile;
original exe and saves untouched). The public repo contains no game assets, extracted payloads, or
decompiled program text; all lane bulk output is under `artifacts/lanes/s1-*/` (gitignored).

Plan of record: [`SPRINT1.md`](SPRINT1.md). Baselines: `docs/SPRINT0_REPORT.md`, `docs/P0_REPORT.md`,
`docs/MASTER_PLAN.md` §6 (P1) and §9 (verification).

## Exit criteria (gate G1)

| # | Criterion | Result | Evidence |
|---:|---|---|---|
| 1 | Resource coverage ledger, zero unhandled types | **met** — 93,053 resources in 402 containers, 3,587,078,005 B, 93,053 ok / 0 failed / 0 open; `zero_unhandled=true`; 18/18 cross-checks vs recon 01/06 | `docs/recon/17_RESOURCE_COVERAGE.md`; `artifacts/lanes/s1-01-coverage/` |
| 2 | Format parser matrix + round-trip | **met** — 75,676/75,676 payload validations pass; byte-identical round-trips: GFF 11,143/11,143, 2DA 519/519 (+ASCII 519/519), TLK 49,265 entries, ERF 5/5, RIM 246/246, BIF 26/26, KEY 1/1; `validate_all.py` exit 0 on the reference install | `docs/recon/18_FORMAT_MATRIX.md`; `artifacts/lanes/s1-02-formats/validate_all.json` |
| 3 | Map compiler, 117 areas, hard-fail rules, frozen contract | **met** — 117/117 module pairs compiled (~116 s); rules R1–R6 fire correctly (synthetic fixtures hard-fail with the right codes; 0 inverted faces in 190,865 shipped WOK faces); shipped-data hard classes triaged (R3 90 rooms, R5 322 entries, R6 6, R4 1) with data-backed explanations; ADR 0002 contract frozen | `docs/recon/19_MAP_COMPILE.md`; `artifacts/lanes/s1-03-mapcompile/` |
| 4 | Script corpus ledger | **met** — 13,139 NCS occurrences → 8,666 unique, 8,666/8,666 byte-identical re-encode (two independent paths agree, 0/8,666 differ vs lane-04 index); NSS pairing 1,680/8,666 (base layer 1,611/1,637 = 98.41%); decompile classes merged 8,666/8,666 (8,428 exact); 5 bug-parity arity anomalies explicit; per-module action usage (117 modules) | `docs/recon/20_SCRIPT_CORPUS.md`; `artifacts/lanes/s1-04-scripts/` |
| 5 | Save/VFS completion | **met** — unmodified re-encode of a retail save slot byte-identical 100% (ERF verbatim 46/46 across 14 real slots, GFF verbatim 100%, whole-slot 5/5); from-scratch ERF writer byte-identical to retail; GFF sidecars field-equal (diff classes classified); all inferred VFS items closed synthetically or handed to s1-06 | `docs/recon/21_SAVE_BYTEPARITY.md`; `artifacts/lanes/s1-05-save-vfs/` |
| 6 | Dynamic P1 measurements | **met with 4 documented blockers** — loaded-save RIM order verified (`HD0:players → LIPS → MODULES:<m>_s → CURRENTGAME:<m>`; `.mod` misses all passes on retail); boot order re-verified; streamwaves scan+index verified (1,144 files, 978 wavs; zero streamwaves `.wav` opens in menu→module→gameplay, all wav opens were streammusic); camera `+0x38 = 0` from controller `+0xc4 = 0` observed live in-process; rewritten save `000007 - PARITY` accepted by the retail engine via the normal Load Game UI | `docs/recon/22_DYNAMIC_P1.md`; `artifacts/lanes/s1-06-dynamic/` |
| 7 | Binary P1 verification (hydralysis / Hydra h5) | **met** — 18 evidence-DB claims (#11–#28) on the sealed master, every address quoted with the build SHA-256: N-group mesh loops proven N-group capable; supermodel chain semantics (duplicate/ancestor-only/missing); camera `+0x38` write set proven construction-zero, mode-nonzero branches unreachable; MDX GPU skinning via `program.env[18..68]`, aux4 proven consumer-less | `docs/recon/23_BINARY_P1.md`; `artifacts/lanes/s1-07-binary/` |
| 8 | First textured tools-side viewer | **met** — `tools/viewer/` renders m01aa (top-down + perspective, 9 TPC textures, 42/42 checks; counts match recon/11) and m09ab (26/26), plus `mainmenu.gui` (9/9) and `maininterface.gui` (31/31) to PNG | `docs/recon/24_VIEWER.md`; `artifacts/lanes/s1-08-viewer/` |
| 9 | Evidence/repo hygiene | **met** — lanes wrote only their own new files; integrator owns edits to existing tracked files and this report; tests green (see below); no assets/secrets in tracked files | this report; `git status` |

## Test totals (integrator re-run on the merged tree)

- `python tools/tests/run_all.py` → **102 tests, OK (1 pre-existing skip: no save in the reference
  install itself)**; `artifacts/tests/results/test-results.json` regenerated.
- `python -m unittest discover -s tools/formats -p "test_*.py"` → **211 tests, OK**.
- `tools/scripts/tests` → **29, OK**; `tools/tests/test_mapcompile.py` → **13, OK**;
  `tools/inventory` tests → **25, OK (1 opt-in skip)**; `tools/tests/test_save_parity.py` 11/11,
  `test_vfs_p1.py` 12/12, `tools/tests/test_viewer.py` 19/19 (all inside run_all or lane suites).
- `python tools/formats/validate_all.py --game-dir "$KOTOR_GAME_DIR"` → **exit 0, `all_ok=true`**
  (86.8 s), ledger at `artifacts/lanes/s1-02-formats/validate_all.json`.

## Key verified facts added this sprint

- **Coverage (G1):** 93,053 resources / 402 containers fully typed, hashed, and validated;
  zero unhandled type ids; 18/18 totals reconcile with the Sprint 0 manifest. BTI/BTC are GFF V3.2.
  26 RIM-copied 2DAs use a NUL-separated V2.b variant (now parsed). 8,290 duplicate (resref,type)
  pairs exist across archives (VFS first-wins rules apply).
- **Round-trip:** GFF, 2DA (V2.b binary + NUL variant + ASCII V2.0), TLK, ERF/MOD, RIM, BIF, KEY all
  re-encode **byte-identical** over the full shipped corpus. Save slots: unmodified re-encode is
  byte-identical end-to-end on a retail save; a from-scratch ERF writer reproduces retail bytes.
- **Map compiler:** resource homes verified (ARE/GIT/IFO in `<mod>.rim`, PTH in `<mod>_s.rim`,
  LYT in `layouts.bif`, VIS in `lightmaps*.bif`, WOK/MDL/MDX in `models.bif`); R1 winding clean on
  all 190,865 shipped WOK faces; shipped hard classes are data quirks, triaged per area
  (multi-level walkmeshes joined by doors/PTH, 51 mm-class door-hook offsets, 0xFEFFEEEE UV sentinel).
  `MapResX` is a resolution hint outside the minimap affine; no `<area>` minimap texture ships —
  the minimap is rasterized from the ARE affine.
- **Scripts:** pairing 19.39% overall (98.41% in the base layer; module scripts ship without
  sources); 466/772 actions used; the 5 arity-anomaly scripts are pinned as bug-parity items.
- **Dynamic:** loaded-save module mount order and boot pass order measured with verbatim hook
  lines; streamwaves is scan+indexed but issues no `.wav` file opens before gameplay voice; camera
  `+0x38 = 0` observed live; the byte-parity rewritten save loads in the retail engine.
- **Binary (Hydra h5):** all four P0-handoff binary items proved with line-cited queries on the
  sealed master; evidence DB claims #11–#28.

## Closure pass (2026-11-01, after the initial commit) — what changed

- **`kotor_vfs.py` defaults fixed to the measured rules** (integrator): flat one-level OS-order
  directory scan (was recursive+sorted), pass-1 last-added-first search (AddKey order Override,
  MOVIES, STREAMWAVES, STREAMMUSIC), measured pass-4 order save > `.mod` > `LIPS:localization` >
  players > `swpc_tex_gui` > quality tier (was packs before lips/players). Pinned by rewritten
  tests in `tools/tests/test_vfs_p1.py` / `test_vfs.py` (+ a `streammusic/wav_probe.wav` fixture
  making the first-launch streamwaves quirk observable). Full suite still green (102 + 211 tests).
- **Synthetic 2-group mesh — verified live** (`docs/recon/25_SYNTHETIC_RUNTIME.md`): a crafted
  group-count-2 `p_carthh` MDL in profile Override was read by the engine, which entered the
  per-group relocation loop at `0x4a0ce0` (`g=2`, fields `+0x104/+0x108/+0x110/+0x114/+0x11c/
  +0x120 = 2`) — N-group capability confirmed at runtime, matching the static proof (#11–13).
- **Supermodel chain — verified live**: 2,918 clip lookups at `0x4822a0` with 579 `super_enter`
  depth-2 recursions into `+0x64`, 32 terminal misses, 154 plays at `0x485bd0`, 1,092 queries at
  `0x484d20` — exactly the statically predicted chain semantics (#14–17).
- **From-scratch parity save slot accepted by the retail engine** (run `h-paritysrc`): the
  `--mode parity` rewrite loaded via the normal Load Game UI; slot unchanged after load.
- **Fresh-module RIM mount order — proved statically** (evidence #23–#25): New Game path
  `FUN_004babb0` → `FUN_004c5720` → mounter `FUN_004c44d0` probes `modules/<mod>.mod` first
  (absent in retail), then mounts `MODULES:<mod>_s` **first**, then LIPS, then conditional
  `_a`/`_adx`, then players. Only `_s` can fire in retail — consistent with the live loaded-save
  run. **Dialogue-start camera path — proved statically** (#26–#28): `+0x38` is construction-zero
  on the dialogue path too (`FUN_0062bf30` → controller `+0xc4` → factory `0x6412f0` → ctor write
  `0x6bb6f8`); CameraAnimation maps via `FUN_006288f0` to `cutNNN` (`camerahook` model).

## Open items carried forward (with owners and blockers)

1. **Chargen PLAY / in-dialogue live runs** — injected input does not reach the chargen 3D screen
   or dialogue targeting under llvmpipe (DirectInput), logged verbatim across distinct approaches
   (posted clicks, SendInput absolute, keyboard traversal, GUI-derived coords, orca screenshots,
   89-click grid). Both items are now **statically bounded** (#23–#28: only `_s`-first mounting and
   camera mode 0 are possible), so the residual is environmental, not a data gap. Owner: P4
   vertical slice on hardware (the P0 elevated 1280×720 session already proved F4/F5 saves work).
2. **streamwaves per-file audio requests** — scan + pass-1 index measured; no voice line could be
   triggered under the input blocker above; zero streamwaves `.wav` opens observed in
   menu→module→gameplay. Owner: P5 audio lane on hardware.
3. **F4 quicksave thumbnail leg under Mesa** — `Interceptor.replace` on `0x420f20` is in place
   (runs k3/k4) but the quicksave stalls upstream of the thumbnail in the non-elevated llvmpipe
   harness; needs the elevated session + GL shims of `docs/recon/16` §6. Owner: P4 renderer
   (save data/reload unaffected; P0 already verified a real F4→F5 round trip).
4. **Map-compile hard classes on shipped data** — lightmap-UV wrap semantics and minimap
   rasterization renderer-side confirmation. Owner: P5/P6 (recon/19 handoff).
5. **GFF sidecar byte-parity** — verbatim re-encode is byte-identical; the from-scratch writer is
   field-equal but not byte-identical (retail does not 4-align fdata; diff classes classified:
   global row interleaving, PARTYTABLE orphaned fidx copies). Owner: P3 save round-trip matrix if
   byte parity of sidecars is ever required.
6. **228 compiler-limited decompiles + 306 unused actions** — engine implementations needed for
   modded content. Owner: P3 VM conformance.

## Reproduce

```bash
export KOTOR_GAME_DIR=/e/SteamLibrary/steamapps/common/swkotor   # read-only; sentinel-checked
python tools/tests/run_all.py
python -m unittest discover -s tools/formats -p "test_*.py"
python tools/formats/validate_all.py --game-dir "$KOTOR_GAME_DIR"     # exit 0, all_ok=true
python tools/inventory/resource_coverage.py --game-dir "$KOTOR_GAME_DIR" # 93,053 ok / 0 open
python -m tools.mapcompile.compile --game-dir "$KOTOR_GAME_DIR"       # 117 areas, per-area JSON
KOTOR_GAME_DIR="$KOTOR_GAME_DIR" python tools/viewer/area_viewer.py --module end_m01aa --out artifacts/lanes/s1-08-viewer
# dynamic lane (consent-gated):
python tools/harness/sprint1_probes.py preflight
python tools/harness/sprint1_dynamic_run.py --consent   # requires artifacts/lanes/s1-06-dynamic/CONSENT
```

## Public repository

Master now carries Sprint 1: `docs/SPRINT1.md` (plan), `docs/SPRINT1_REPORT.md` (this report),
`docs/recon/17`–`24`, and the new tool packages `tools/formats/*` (parser matrix + writers +
`validate_all.py`), `tools/inventory/resource_coverage.py`, `tools/mapcompile/`, `tools/scripts/`,
`tools/resources/kotor_save_parity.py`, `tools/harness/sprint1_*`, `tools/viewer/`, plus their
tests. No assets, saves, extracted payloads, or decompiled program text are tracked; `artifacts/`
remains local. Next phase: P2 binary map continuation and P3 VM conformance on top of this data
foundation; P4 vertical slice can start from the frozen map-compile contract.
