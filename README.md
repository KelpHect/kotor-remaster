# KotOR 1 Interoperability Runtime (unofficial)

Status: **Sprint 3 (P3 functional NCS runtime and P3/G3 residual closure) complete — gate S3 met
(2026-11-02).** There is still no playable engine build; P4/P5 renderer and gameplay work
continues on the frozen data foundation and the executable NCS runtime. Sprint plans/reports:
`docs/SPRINT1.md` / `docs/SPRINT1_REPORT.md`, `docs/SPRINT2.md` / `docs/SPRINT2_REPORT.md`, and
`docs/SPRINT3.md` / `docs/SPRINT3_REPORT.md`; the P0 gate and handoff remain in
`docs/P0_REPORT.md`.

The runtime is intended to load a user-supplied KotOR 1.0.3.0 installation and run it in place. It
does not ship, download, extract, or redistribute game assets, and it does not require or execute
the original game executable.

Target architecture: original C++23 runtime, SDL3 platform, Vulkan 1.4 primary RHI with an OpenGL
4.5 reference backend; a Classic path for 1:1 behavior and an optional Remaster presentation path.

## Verified progress

- Fresh Ghidra 12.1.4 analysis of a user-owned 1.0.3.0 build.
- Full pseudocode export for the v4 inventory: **10,849/10,849 internal functions decompiled**,
  0 failures, 0 missing/extra/duplicate or overlapping bodies, with assembly/body/pseudocode hashes
  (350 external imports included; ledger 10,849/10,849).
- Script action table: **772/772 actions mapped** and **565/565 handler targets named**; 440
  handler functions + dispatcher were added and decompiled in an isolated project copy, then the
  last 125 `FUN_` handlers were renamed (9,057 internal functions; 350 external imports).
- Archive inventory: 25,836 base BIF resources, 20,575 module RIM resources, 1,936 global-RIM
  resources, plus patch/texture-pack ERFs; 49,265 TLK strings.
- TLK V3.0 parser validated; SQLite evidence ledger; action-table extraction tooling.
- Full install manifest: 15,380 files / 3.36 GiB hashed without modifying the install.
- 2DA parser validates all 209 shipped tables (7,750 rows, 111,521 cells, zero failures).
- GFF V3.2 wire semantics verified; the single-field struct rule is implemented and IFO/GIT/ARE
  validate. Coordinate probes on m01aa establish RH Z-up, 1 unit = 1 m, IFO ground-point/unit-dir,
  creature forward vectors, radian Bearing, world-space WOK, and the LYT/MDL transform chain;
  Bearing sign and MDX layout are closed at the shipped-code boundary
  (`docs/recon/03`, `docs/recon/04`, `docs/recon/15`).
- GFF corpus validated: 11,143/11,143 shipped payloads parse, 1,854,982/1,854,982 values resolve,
  zero intrinsic failures; the struct-slot packing invariant was disproved by shipped files and
  replaced with disjoint-range checks (`docs/recon/07`, 30 regression tests).
- Script corpus: 8,666 unique NCS (13,139 occurrences) all decode; 285,015 ACTION instructions
  reference only known action IDs; 466/772 actions are used. Clean-room decompilation reaches
  **8,428 exact** byte-identical/full-stream matches (97.26%), with 228 compiler-limited, 0
  decompiler-side open, 5 header-limited, and 5 reachable-only; 0 partial, 0 regressions
  (`docs/recon/09`).
- Save/VFS: save container parser/validator and structural writer, archive readers/writers, and a
  five-pass resolver, with 47 synthetic/real-install tests (46 pass, 1 skip). Dynamic hooks confirm
  the pass order,
  Override precedence, and quality 2/1/0 → tpa/tpb/tpc. The closing pass measured duplicate-in-
  archive and KEY first-wins, `.mod`/RIM, save-vs-module, pass-3/4 order (save > `.mod` > players >
  texture packs), and flat one-level loose directories. Retail save acceptance is verified at a
  true 1280×720 windowed client with the permanent windowed mod: the vanilla Taris Hideout save
  loads to the HUD, F4 writes a valid `000000 - QUICKSAVE` (`last_module=tar_m02af`), and F5
  quickload restores it. The earlier black screen was Frame Buffer Effects; `Frame Buffer=0`
  removes the 120×120 Mesa pbuffer path. The save-thumbnail resampler still needs a renderer shim
  under Mesa. See `docs/recon/08`, `docs/recon/10`, `docs/recon/16`.
- Sprint 1 (P1, gate G1): 93,053/93,053 shipped resources typed, hashed, and validated with zero
  unhandled types and 18/18 manifest cross-checks; byte-identical round-trips for GFF (11,143),
  2DA (519, incl. the NUL-separated RIM variant and ASCII V2.0), TLK (49,265), ERF (5), RIM (246),
  BIF (26), KEY, and whole retail save slots (the from-scratch parity rewrite is 5/5 files + nested
  1/1 byte-identical); map compiler over all 117 areas, **exit 0** (4 clean, 113 triaged, 0 hard)
  with strict-mode hard-fail rules R1–R6 and the ADR 0002 coordinate contract frozen; script corpus
  ledger (8,666 unique NCS, 8,666/8,666 lossless re-encode, 8,428 exact decompiles, NSS pairing,
  per-module action usage); retail save rewrite accepted by the original engine; live measurements
  of module mount order, streamwaves scan/index, and camera `+0x38 = 0`; Hydra h5 evidence claims
  #11–#32 for the P0 binary handoff, GFF writer layout, and streamwaves voice path; tools-side
  textured area/GUI viewer.
  See `docs/recon/17`–`25`
  and `docs/SPRINT1_REPORT.md`.
- Sprint 2 (P2 G2 + P3 VM core): **772/772 action specs** with return types and typed stubs;
  engine map with 10,302 engine functions, 0 unknown, 0 unexplained `.text`, dispatcher
  `0x0052c0d0`, 565/565 handler targets; an independent NCS VM executed **8,666/8,666** unique
  shipped scripts with 0 decode errors, 0 VM faults, 0 unknown actions, and 0 invalid jump
  targets (8,184 returned, 472 blocked at `ActionWait`, 10 budget), and all 772 actions pass a
  direct typed-dispatch check. Tests: 129 OK (1 skip).
  See `docs/recon/26_ENGINE_MAP.md`, `docs/recon/27_VM_EXECUTION.md`, and
  `docs/SPRINT2_REPORT.md`.
- Sprint 3 (P3 functional NCS runtime and P3/G3 residuals): deterministic object/tag/area,
  local/global, inventory/party, action-queue, and timer model; **344 corpus action ids** covered
  by 44 specific + 300 generic handlers with **0 unclassified**; full corpus **8,666/8,666** with
  0 decode/VM faults, 303,616 ACTION calls, 20,406 timers, and 5,401 queued actions. **Live
  `ExecuteCode`/action-dispatch differential: 28/28 unique programs, 400 executions, 12,000
  ACTION calls, exact id/IP/SP/BP/return-cell match; the `RunCommand` hook (`0x005d45d0`)
  captured 4,000 queued-command calls and its 30 unique programs also replay 30/30.** **228/228
  compiler-limited recompiles byte-identical: 24/24 source-level historical headers, 201/201
  structs recovered source-level (2 fully exact, 199 one compiler cleanup block), 3 body-order
  ABI; full 8,666 rerun 8,459 raw exact, 202 compiler-limited (199 cleanup + 3 body), 5
  reachable-only.** **306/306 corpus-unused actions** behaviorally tested against a prepared
  world (42 specific + 264 generic, 0 default, 82 state-derived assertions + 20 SWMG set/get
  pairs). Tests: 144 OK
  (1 skip) + 36 script tests. See `docs/recon/28_ACTION_MODEL.md`,
  `docs/recon/29_FUNCTIONAL_RUNTIME.md`, `docs/recon/30_UNUSED_ACTIONS.md`,
  `docs/recon/31_COMPILER_CODEGEN.md`, `docs/recon/32_LIVE_DIFFERENTIAL.md`,
  `docs/recon/33_HISTORICAL_HEADERS.md`, `docs/recon/34_ACTION_BEHAVIOR.md`,
  `docs/recon/35_FULL_RECOMPILE.md`, `docs/recon/36_STRUCT_RECOVERY.md`, and
  `docs/SPRINT3_REPORT.md`.
- Install validation/cache: read-only dataset fingerprinting, external content-addressed cache
  profile, and a manifest-reconciled install (0 file/size deltas); 14 tests (`docs/recon/13`).
- Function classification: **11,199 items all high-confidence** — 10,302 engine, 441 CRT,
  350 external imports, 76 thunks, 30 compiler helpers, 0 unknown; 34/34 checks, 0 unexplained
  `.text` bytes (`docs/recon/14`). Bearing `R_z(+B)` is proven in engine code at
  `0x50a0e0`/`0x50a7b0` → `0x4ac960` → `0x5853c0`.
- MDX layout decoded across the full corpus: 2,832/2,832 models, 76,703 blocks, 489,549,652 bytes
  exactly assigned to payload/sentinel/pad; relocation `runtimePtr = dataHeader + word`, GPU
  qbone/tbone skinning via `program.env[18..68]`, and aux4 is dead exporter data with no shipped
  consumer. All 76,703 blocks are group count 1; bbox expansion is exporter/scene-time
  (`docs/recon/15`).
- Animation/camera: type 8 position is additive to bind, type 20 orientation is absolute, no NaN
  sentinel; `WOK.position` is room walkmesh node 545 translation (453/453 bit-exact); DLG
  `CameraAnimation` = `1200+(N−1)` → `cutNNNw`; `CameraAngle` ABI/framing decoded; supermodel
  precedence and supernode binding verified; camera `+0x38` is construction-time zero from
  controller `+0xc4` (live probe confirmed `+0xc4 = 0` in-process), `+0x34` is a per-update
  coincident flag, and obstruction segments/clip paths are mapped (`docs/recon/12`, `15`).
- Engine scaffold: C++23 + SDL3 + Vulkan 1.4 (1.3 floor) + OpenGL 4.5 + ImGui builds from
  `engine/` with `/W4 /WX`; Vulkan and OpenGL both run 120 frames, headless CTest 2/2. This is a
  clear/ImGui presentation scaffold, not a renderer yet (`engine/docs/BUILD.md`).
- m01aa geometry path: LYT/WOK/MDL/PTH/IFO/GIT parse into a top-down preview with 1,365 WOK
  triangles, 15 doors/hooks, 77 PTH nodes, and 45/45 cross-checks (`docs/recon/11`).
- Architecture decisions for the runtime/renderer, coordinate + map-compilation contract, and the
  user-supplied-install distribution model.

## Repository layout

```
engine/      C++23 runtime scaffold: SDL3, Vulkan 1.4 / OpenGL 4.5, ImGui
docs/        master plan, reconnaissance, ADRs, format/VM notes
tools/       read-only inventory/parsers, Ghidra exporters, action-table and evidence tooling
artifacts/   local only (gitignored): Ghidra projects, exports, caches, captured evidence
```

Start with:

- [`engine/docs/BUILD.md`](engine/docs/BUILD.md) — build the engine scaffold and run its smoke tests
- [`docs/MASTER_PLAN.md`](docs/MASTER_PLAN.md) — program lanes, phases, gates, verification
- [`docs/SPRINT3_REPORT.md`](docs/SPRINT3_REPORT.md) — Sprint 3 (P3 functional runtime) exit report
- [`docs/SPRINT3.md`](docs/SPRINT3.md) - Sprint 3 plan: exit criteria, lanes, shared-resource rules
- [`docs/recon/30_UNUSED_ACTIONS.md`](docs/recon/30_UNUSED_ACTIONS.md) - 306 corpus-unused action conformance
- [`docs/recon/31_COMPILER_CODEGEN.md`](docs/recon/31_COMPILER_CODEGEN.md) - 228 compiler-limited recompile reconciliation
- [`docs/recon/32_LIVE_DIFFERENTIAL.md`](docs/recon/32_LIVE_DIFFERENTIAL.md) - live ExecuteCode/action-dispatch and RunCommand differential
- [`docs/recon/33_HISTORICAL_HEADERS.md`](docs/recon/33_HISTORICAL_HEADERS.md) - source-level historical prototype headers (24 cases)
- [`docs/recon/34_ACTION_BEHAVIOR.md`](docs/recon/34_ACTION_BEHAVIOR.md) - behavioral conformance for the 306 unused actions
- [`docs/recon/35_FULL_RECOMPILE.md`](docs/recon/35_FULL_RECOMPILE.md) - full 8,666-script source recompile ledger
- [`docs/recon/36_STRUCT_RECOVERY.md`](docs/recon/36_STRUCT_RECOVERY.md) - source-level `tLastRound` struct recovery (201 files)
- [`docs/SPRINT2_REPORT.md`](docs/SPRINT2_REPORT.md) — Sprint 2 (P2 G2 + P3 VM core) exit report
- [`docs/SPRINT2.md`](docs/SPRINT2.md) — Sprint 2 plan: exit criteria, lanes, shared-resource rules
- [`docs/SPRINT1_REPORT.md`](docs/SPRINT1_REPORT.md) — Sprint 1 (P1) exit report and gate G1
- [`docs/SPRINT1.md`](docs/SPRINT1.md) — Sprint 1 plan: exit criteria, lanes, shared-resource rules
- [`docs/SPRINT0_REPORT.md`](docs/SPRINT0_REPORT.md) — Sprint 0 exit report, lane results, and gaps
- [`docs/P0_REPORT.md`](docs/P0_REPORT.md) — P0 foundation exit report and Gate G0
- [`docs/SPRINT0.md`](docs/SPRINT0.md) — current foundation sprint exit criteria and lane board
- [`docs/recon/01_ARTIFACTS.md`](docs/recon/01_ARTIFACTS.md) — verified artifact/content inventory
- [`docs/recon/02_TECHNICAL_NOTES.md`](docs/recon/02_TECHNICAL_NOTES.md) — format, VM, VFS, rules notes
- [`docs/recon/03_GFF_COORDINATES.md`](docs/recon/03_GFF_COORDINATES.md) — verified GFF wire semantics and coordinate evidence
- [`docs/recon/04_GEOMETRY_COORDINATES.md`](docs/recon/04_GEOMETRY_COORDINATES.md) — IFO/WOK/LYT/MDL coordinate contract and fixtures
- [`docs/recon/05_2DA.md`](docs/recon/05_2DA.md) — 209-table 2DA coverage
- [`docs/recon/06_MANIFEST.md`](docs/recon/06_MANIFEST.md) — complete install manifest summary
- [`docs/recon/07_GFF_CORPUS.md`](docs/recon/07_GFF_CORPUS.md) — all-resource GFF validation and corrected invariants
- [`docs/recon/08_VFS_DYNAMIC.md`](docs/recon/08_VFS_DYNAMIC.md) — five-pass VFS order, precedence, and texture-quality mapping, measured in-process
- [`docs/recon/09_SCRIPTS.md`](docs/recon/09_SCRIPTS.md) — 8,666-script NCS corpus, validation, action usage, and decompile coverage
- [`docs/recon/10_SAVE_VFS.md`](docs/recon/10_SAVE_VFS.md) — save containers, archive I/O, and the five-pass VFS resolver
- [`docs/recon/11_GEOMETRY_PREVIEW.md`](docs/recon/11_GEOMETRY_PREVIEW.md) — m01aa LYT/WOK/MDL/PTH/GIT top-down preview and checks
- [`docs/recon/12_COORDS_OPEN_ITEMS.md`](docs/recon/12_COORDS_OPEN_ITEMS.md) — corpus-scale Bearing, camera, ARE NorthAxis, and remaining open items
- [`docs/recon/13_P0_INSTALL.md`](docs/recon/13_P0_INSTALL.md) — install validation, external cache profile, and the config-restore guard
- [`docs/recon/14_P0_FUNCTION_CLASSIFICATION.md`](docs/recon/14_P0_FUNCTION_CLASSIFICATION.md) — exhaustive 11,199-row engine/CRT/middleware/thunk classification
- [`docs/recon/15_MDX_LAYOUT.md`](docs/recon/15_MDX_LAYOUT.md) — verified MDX block/vertex/face layout and walkmesh correspondence
- [`docs/adr/0001-runtime-and-renderer.md`](docs/adr/0001-runtime-and-renderer.md)
- [`docs/adr/0002-coordinates-and-render-features.md`](docs/adr/0002-coordinates-and-render-features.md)
- [`docs/adr/0003-distribution-and-install-model.md`](docs/adr/0003-distribution-and-install-model.md)
- [`tools/README.md`](tools/README.md) — tool usage and toolchain

## Legal and scope

This is an unofficial, non-commercial interoperability/research project. It is not affiliated with
or endorsed by BioWare, EA, Lucasfilm, Disney, or any platform holder. No game assets, extracted
data, save files, or decompiled program text are committed here.

You must supply your own legally acquired game installation. The project does not implement DRM,
license checks, or ownership verification. External GPL/LGPL tools are used as independent oracles
or authoring tools only; their code is not linked into the engine. A project license has not been
selected yet; no license is granted until one is added. Nothing in this repository is legal advice.

## Contributing

Evidence first: claims about the binary or formats must cite a build hash, address/offset, and
tool/test evidence. Keep game-derived artifacts out of commits, and keep implementation code
independent of GPL prior-art source.
