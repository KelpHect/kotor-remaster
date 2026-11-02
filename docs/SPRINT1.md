# Sprint 1 — Data completeness and map-compile freeze (phase P1, gate G1)

Status: **complete — gate G1 met** — see [`SPRINT1_REPORT.md`](SPRINT1_REPORT.md) for the exit
report, carried open items, and reproduce instructions.
Goal: reach **100% resource parse coverage with zero unhandled types**, a green byte/semantic
round-trip suite, a map compiler that validates all 117 areas with hard-fail rules, a completed
script-corpus ledger, byte-parity save writing, and closure of the P1 items handed over from P0
(`docs/P0_REPORT.md` → "P1/P4 handoff"). Engine rendering/gameplay parity stays in P4/P5.

Baseline: Sprint 0 / P0 strict closure (`docs/SPRINT0_REPORT.md`, `docs/P0_REPORT.md`),
master plan phase P1 (`docs/MASTER_PLAN.md` §6), ADRs 0001–0003.

## Exit criteria (gate G1)

1. **Resource coverage ledger** — every resource in the 26 base BIFs (25,836), 234 module RIMs
   (20,575), 12 global RIMs (1,936), `patch.erf` (97), four texture ERFs (11,453), plus loose
   files is typed, parsed, hashed, and ledgered; **zero unhandled types**; ledger and report at
   `artifacts/lanes/s1-01-coverage/` + `docs/recon/17_RESOURCE_COVERAGE.md`.
2. **Format parser matrix + round-trip** — a parser per shipped resource type (GFF all variants,
   2DA V2.b/V2.0 binary+ASCII, TLK V3.0, ERF/MOD/SAV, RIM, BIF/KEY, LYT, WOK, BWM, PTH, VIS,
   MDL/MDX, TPC/TXI, DDS, TGA, SSF, LIP, WAV/MP3, BIK, GUI, NCS, NSS, INI) under `tools/formats/`;
   byte-identical round-trip writers for GFF/ERF/MOD/RIM/2DA/TLK over the full corpus;
   `tools/formats/validate_all.py --game-dir <install>` green; `docs/recon/18_FORMAT_MATRIX.md`.
3. **Map compiler** — all 117 areas compiled from original LYT/WOK/BWM/PTH/VIS/GIT/ARE/IFO by
   `tools/mapcompile/`; the validator hard-fails on bad winding, out-of-bounds geometry, walkmesh
   or PTH connectivity breaks, lightmap UVs outside [0,1], and misplaced anchors/doors; landmark
   fixtures freeze the coordinate/transform contract (ADR 0002); per-area reports at
   `artifacts/lanes/s1-03-mapcompile/`, doc `docs/recon/19_MAP_COMPILE.md`.
4. **Script corpus** — NSS↔NCS pairing ledger for all 8,666 unique NCS, per-file decompile
   status, and per-module action-usage tables; `tools/scripts/`;
   `docs/recon/20_SCRIPT_CORPUS.md`.
5. **Save/VFS completion** — save writer that round-trips a retail save byte-compatibly (original
   → parse → write → original accepts), and the VFS discriminating-asset harness extended to
   close every "inferred" item left in `docs/recon/10_SAVE_VFS.md`; `docs/recon/21_SAVE_BYTEPARITY.md`.
6. **Dynamic P1 measurements** (one owned game process, display consent required) — fresh-module
   area-RIM intra-order (`_a`/`_adx`/`.rim`/`_s` and `.mod` vs area RIM), `streamwaves` audio
   requests, and the dialogue/cutscene camera-object field observed in-process;
   `docs/recon/22_DYNAMIC_P1.md`.
7. **Binary P1 verification (hydralysis / Hydra Engine h5)** — static+dynamic evidence for the
   N-group mesh loops, the supermodel child/super visual chain, camera `+0x38`, and MDX GPU
   consumers, recorded in the evidence DB with the build SHA-256
   `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`;
   `docs/recon/23_BINARY_P1.md`.
8. **First textured viewer (tools-side)** — geometry+texture+GUI viewer reading original data:
   render at least m01aa and one other area with TPC textures, and load a `.gui` screen;
   `tools/viewer/`, `docs/recon/24_VIEWER.md`.
9. **Evidence/repo** — ledger updated, lane outputs integrated, no game assets or decompiled
   program text in tracked files, `docs/SPRINT1_REPORT.md` written with the G1 verdict.

## Lanes (wave 1)

| Lane | Scope | Owns (new files only) | Output root |
|---|---|---|---|
| s1-01-coverage | G1 coverage ledger over every shipped resource | `tools/inventory/resource_coverage.py`, `docs/recon/17_*.md` | `artifacts/lanes/s1-01-coverage/` |
| s1-02-formats | parser matrix + round-trip writers + validate_all | `tools/formats/<new>.py`, `tools/formats/test_<new>.py` | `artifacts/lanes/s1-02-formats/`, `docs/recon/18_*.md` |
| s1-03-mapcompile | map compiler + validator + landmark fixtures, 117 areas | `tools/mapcompile/`, `docs/recon/19_*.md` | `artifacts/lanes/s1-03-mapcompile/` |
| s1-04-scripts | NSS↔NCS pairing, decompile status, per-module action usage | `tools/scripts/`, `docs/recon/20_*.md` | `artifacts/lanes/s1-04-scripts/` |
| s1-05-save-vfs | byte-parity save writer + VFS closing fixtures | new files in `tools/resources/`, `tools/tests/test_save_parity.py`, `docs/recon/21_*.md` | `artifacts/lanes/s1-05-save-vfs/` |
| s1-06-dynamic | one owned game process: RIM intra-order, streamwaves, dialogue camera | `tools/harness/` new scripts, `docs/recon/22_*.md` | `artifacts/lanes/s1-06-dynamic/` |
| s1-07-binary | hydralysis/hydra-h5 evidence: mesh groups, supermodel chain, camera +0x38 | `docs/recon/23_*.md`, evidence DB entries only | `artifacts/lanes/s1-07-binary/` |
| s1-08-viewer | tools-side textured geometry + GUI viewer | `tools/viewer/`, `docs/recon/24_*.md` | `artifacts/lanes/s1-08-viewer/` |

## Shared-resource rules

Carried over from Sprint 0 (`docs/SPRINT0.md`), plus:

- **Game process:** only lane s1-06 launches/drives the original, one owned instance at a time,
  windowed (`FullScreen=0`), profile-local working directory, install untouched (read-only hash
  sentinel check, stop on drift). **Display consent:** without explicit foreground consent from
  the user in the current session, do not launch on the interactive desktop; use a verified
  isolated virtual screen without changing focus, or defer the lane and mark its items blocked.
  Capture only the identified game window. Never bypass DRM/ownership checks; never modify or
  overwrite original saves — use a fresh slot.
- **Hydralysis / Hydra (h5 at the time; h6 since 2026-11-02, see
  `docs/recon/40_HYDRA_UPGRADE.md`):** the sealed master of target `kotor` (Hydra Engine
  `tools/hydra-root/`) is read-only for everyone. Only lane s1-07 calls `evidence_add`, and each
  editing agent must pass a distinct `options.as` view id. Never run `prepare`/import; the target
  is already adopted. Quote the build SHA-256 with every address. Runtime attach/spawn belongs to
  lane s1-06 only (single game-process lock).
- **Repository:** lanes write only their own new files (table above); the integrator owns
  edits to existing tracked files, `tools/README.md`, top-level docs, commits, and the report.
  No game data, extracted payloads, or decompiled program text in tracked files; lane outputs
  stay under `artifacts/` (gitignored).
- **Game data reads:** all lanes read the install at `E:\SteamLibrary\steamapps\common\swkotor`
  (Git Bash: `/e/SteamLibrary/steamapps/common/swkotor`) read-only via `KOTOR_GAME_DIR`.
- **Evidence:** every claim needs an address/offset/file hash/test and a
  verified/inferred/open status; cite hydralysis line output verbatim where used.
- **Python:** use `python` (3.15) from Git Bash; run tests with `python tools/tests/run_all.py`
  plus the lane's own unittest modules.
- **Stop rule:** three identical failed attempts → write the recon doc with exact log lines and
  mark the item open; do not thrash.

## Out of scope for Sprint 1

Engine mesh/depth renderer, Vulkan parity rendering, gameplay simulation (P4/P5), VM conformance
suite (P3), full binary engine map beyond the P1 handoff items (P2 continues), license/release
decisions (P6). The save-thumbnail renderer shim stays a P4 item unless it blocks a Sprint 1
exit criterion.
