# KotOR 1 Interoperability Runtime (unofficial)

Status: **Sprint 13 (audio, media, GUI, localization and minigames) complete - gate
S13 met (2026-11-04).** Shipped `streamwaves`, `streamsounds` and `streammusic`
audio decodes (PCM 8/16, IMA ADPCM, MP3, KotOR `0x1d6` prefix); a 48 kHz
stereo mixer provides music/ambient/effects/voice buses, per-voice
volume/pitch/pan/loop, listener-relative 3D attenuation and `soundeax.2da`
reverb presets; GIT `AreaProperties` drive area music/ambience; 45 Endar Spire
UTS sound emitters load and play; DLG `Sound` fields play voice-over; the
script sound/music/ambient actions are wired; SSF sound sets, footsteps and
barks play; all 61 Bink containers and 18,206
LIP timelines validate, native FFmpeg pixel decode reproduces the recorded
640x272 RGBA SHA-256 and presents the movie on GL and Vulkan, Bink Audio
matches the FFmpeg PCM SHA-256 and follows the movie clock, dialogue viseme
sampling is wired, and a procedural 16-viseme facial rig drives the shipped
jaw/mouth/brow bones from the LIP clock; all 86 unique `.gui` screens (2,383
controls, 8 types) parse and render interactively (hit-testing, control state,
actions, screen stack and panel mapping); TLK language selection and localized
voice/LIP paths pass their gate; deterministic pazaak, swoop and
turret state machines pass their
seeded gates; SDL3 pulls the mixer for live playback while headless tests use
the same mixer deterministically. **41/41 CTest targets** pass.
Sprint 12 remains complete (creature animation, skinning, weapons and cutscene
cameras: MDL clips/controllers/skinning, `pause1`/`pause` playback, weapon hand
hooks, GIT/DLG cameras with `CUT###W` clips and height presets, dangly/saber
geometry, full light/emitter controller parameter model, opaque/alpha/additive
FX blending, transition cross-fade). Sprint 11 remains complete (44 Force powers, 125 feat rows, combat/Force/
alignment core, effect stack with Breach/Suppress/Immunity, Jedi Defense,
Force Camouflage, passive formulas and save persistence). Sprint 10 remains
complete (delayed/assigned closures, nested delays, `ActionWait`, `OBJECT_SELF`,
world-clock timers). Sprint 9 remains complete (KotOR 1/K2 profiles and native
original `SAVEGAME.sav` read/write with independent Python validation
**0 errors / 0 warnings** on round-trip and fresh-create slots). Sprint 8.5
remains complete (container loot, companion melee AI, merchant store
gold/identify); Sprint 8 remains complete (117-module sweep, encounter
reset/respawn, formation/leader, feat/power selection); Sprint 7 remains
complete (7 stores/41 `keblastore` entries, 4 encounter volumes/8 spawns, 80%
companion XP, systems UI); Sprint 6 remains complete (native script VM
8,666/8,666 default and functional, 303,616 ACTION calls, dialogue entry 54,
d20 rounds); Sprint 5 remains complete (playable `end_m01aa` slice, 0.03/255
parity). Sprint 14+ remains: non-default `CamVidEffect` semantics (none in the
K1 corpus), mods/Override, KotOR 2 world loading, faction
reputation simulation and the P6 content checklist.
Plans/reports: `docs/SPRINT1.md` through `docs/SPRINT14.md` and their
`*_REPORT.md`; P0 gate in `docs/P0_REPORT.md`.

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
  (`docs/recon/09`). All 228 compiler-limited and 5 reachable-only files are closed by the
  Sprint 3 source-level fixes.
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
  compiler-limited recompiles source-level byte-identical, 0 ABI: 24 historical headers, 201/201
  structs (189 duplicate-return + 12 historical prototypes), 3 body-order via the historical
  compiler; full 8,666 rerun 8,666/8,666 raw exact.** **306/306
  corpus-unused actions** behaviorally tested against a prepared world (42 specific + 264
  generic, 0 default, 82 state-derived assertions + 20 SWMG set/get pairs). Tests: 144 OK
  (1 skip) + 39 script tests. See `docs/recon/28_ACTION_MODEL.md`,
  `docs/recon/29_FUNCTIONAL_RUNTIME.md`, `docs/recon/30_UNUSED_ACTIONS.md`,
  `docs/recon/31_COMPILER_CODEGEN.md`, `docs/recon/32_LIVE_DIFFERENTIAL.md`,
  `docs/recon/33_HISTORICAL_HEADERS.md`, `docs/recon/34_ACTION_BEHAVIOR.md`,
  `docs/recon/35_FULL_RECOMPILE.md`, `docs/recon/36_STRUCT_RECOVERY.md`,
  `docs/recon/37_RETURN_CLEANUP.md`, `docs/recon/38_BODY_ORDER.md`,
  `docs/recon/39_UNREACHABLE.md`, `docs/recon/40_HYDRA_UPGRADE.md`, and
  `docs/SPRINT3_REPORT.md`.
- Sprint 6 (P5 script runtime and systems core): native C++ NCS decoder/VM and a generated 772-row
  action table; **8,666/8,666 default and functional corpus parity with 0 mismatches** against the
  regenerated oracles, **303,616 ACTION calls**, 0 decode/VM/unknown-action faults. The engine loads
  **284 script hooks on 54 entities** plus ARE/IFO hooks, runs spawn/enter/heartbeat/trigger/death/
  placeable scripts, and resolves dialogue `StartingList.Active` conditions (the real Trask
  conversation selects entry 54). 2DA-driven character sheet (Soldier hp 12, BAB 1, Fort 2, AC 12,
  attack +4, level 2 at 1,000 XP), 6-second d20 rounds, party, inventory/equipment, journal, gold,
  and save/load of the character and script state. `ctest`: **17/17 passed**; Python tools: **143
  tests, 7 skips**; install sentinel: 15,380 files unchanged.
  See `docs/recon/43_SCRIPT_AND_RULES.md` and `docs/SPRINT6_REPORT.md`.
- Sprint 7 (P5 systems parity II): UTM/UTI merchant data with **7 stores** in `tar_m02ac`
  (`keblastore` 41 entries, MarkUp/MarkDown 100/40), stock-aware buy/sell and merchant save deltas;
  GIT/UTE encounters with **4 volumes** and **8 spawned creatures** in `tar_m03aa` (meshes,
  scripts, hostile combat stats); companion sheets with the **80% XP share**; character, inventory,
  party, journal and merchant ImGui panels with real Vulkan captures; save schema 3. `ctest`:
  **21/21 passed**; Python tools: **143 tests, 7 skips**; VM parity still 0 mismatches; scene
  parity 0.03/255; install 15,380 files unchanged.
  See `docs/recon/44_SYSTEMS_UI.md` and `docs/SPRINT7_REPORT.md`.
- Sprint 8 (open-work closeout): all **117/117 modules** load with 0 failures and 0 warnings
  (`kotor_all_modules`); encounter `Reset`/`Respawns`/`ResetTime` simulation (real 30 s respawn in
  `tar_m03ab`); companion formation/leader switching; feat and Force-power selection in the
  character panel. CTest: **23/23 passed**; VM parity still 0; scene parity 0.03/255; install
  15,380 files unchanged. See `docs/recon/45_CLOSEOUT.md` and `docs/SPRINT8_REPORT.md`.
- Sprint 8.5 (residual inherited systems): UTP `ItemList` container loot with a Loot panel and
  Take All; companion melee AI that assists the player's target; merchant store gold, identify
  pricing and their save round-trip. CTest: **24/24**; 117-module sweep still 0/0; VM parity 0;
  scene parity 0.03/255; install unchanged. See `docs/recon/46_RESIDUAL_SYSTEMS.md` and
  `docs/SPRINT8_5_REPORT.md`.
- Sprint 9 (original saves and game profiles): `GameProfile` recognizes K1
  (`0290889e...`) and K2 (`6a522e71...`); native original-save reader and writer
  (`SAVEGAME.sav` outer/nested ERFs, PT/GVT/NFO/INV, equipment, party, journal and GIT
  object state, preserved visited-module ERFs and REPUTE); independent Python validation
  **0 errors / 0 warnings** on round-trip and fresh-create, 0 new errors on the party slot;
  source hashes unchanged. CTest: **33/33**; 117-module
  sweep 0/0; VM parity 0; scene parity 0.0300; Python 143 tests / 7 skips; install
  unchanged. See `docs/recon/47_ORIGINAL_SAVES.md` and `docs/SPRINT9_REPORT.md`.
- Sprint 10 (VM/script closure): `STORESTATE` closures for `DelayCommand`/`AssignCommand`,
  nested delays, `ActionWait` continuations, target-aware `OBJECT_SELF`, world-clock timers
  and a queued-functor drain.
  Custom fixture + shipped `k_pend_time01` pass; CTest **34/34**; 117-module sweep 0/0; VM
  parity 0; scene parity 0.0300; Python 143 tests / 7 skips; install unchanged. See
  `docs/recon/48_FUNCTORS.md` and `docs/SPRINT10_REPORT.md`.
- Sprint 11 (combat/Force/alignment): 44 Force powers with FP cost/alignment/range and
  save-based damage/heal/stun/fear/buffs; shared effect stack with Force Immunity, Breach,
  Suppress Force and save/load persistence; 125 feat rows; Power Attack/Power Blast, Flurry,
  Critical Strike, Whirlwind, Force Jump and Jedi Defense deflection; exact skill/conditioning/
  Force Focus/Sense/Scoundrel's Luck/Sneak Attack/Toughness/Wookiee formulas; Force Camouflage
  stealth entry; armor/weapon proficiency; script action/talent bridge; alignment, FP and effects
  round-trip in engine and original saves.
  CTest **35/35**; 117-module sweep 0/0; VM parity 0; scene parity 0.0300; Python 143 tests /
  7 skips; install unchanged. See `docs/recon/49_COMBAT_FORCE.md` and
  `docs/SPRINT11_REPORT.md`.
- Sprint 12 (animation/skinning/weapons/cameras): MDL animation table/clips/
  controllers, packed and float orientations, Bezier position, skin headers,
  node-to-bone maps, `qbone`/`tbone` bind-inverse data, MDX weights/bone ids,
  CPU skinning, per-entity playback with `pause1`/`pause` defaults,
  `animations.2da` and `PlayAnimation`/`ActionPlayAnimation`, equipped weapon
  models on hand hooks, dangly constraint settle, SaberHeader geometry and
  light/emitter billboard quads with opaque/alpha/additive batch blending in
  GL and Vulkan, transition cross-fade and root-motion extraction, GIT cameras
  with DLG `CameraID` fallback, `CameraAnimation` → `CUT###W` camera-MDL
  playback and `CameraAngle` height presets, GL/Vulkan dynamic vertex refresh.
  CTest **36/36**; 117-module sweep 0/0; VM parity 0; scene parity 0.0449;
  Python 143 tests / 7 skips; install unchanged. See
  `docs/recon/50_ANIMATION_SKINNING.md` and `docs/SPRINT12_REPORT.md`.
- Sprint 13 (audio/media/GUI/localization/minigames): PCM/IMA ADPCM/MP3 decode with KotOR prefix handling,
  48 kHz four-bus mixer, 3D attenuation/pan, `soundeax.2da` reverb presets,
  GIT `AreaProperties` music/ambience, 45 UTS sound emitters, DLG voice-over,
  SSF sound sets/barks, appearance-driven footsteps, Bink container/movie
  state, native FFmpeg Bink pixel decode with the recorded RGBA SHA-256 and
  GL/Vulkan movie presentation, Bink Audio decode with the FFmpeg PCM SHA-256
  and movie-clock sync, LIP viseme timelines, a procedural facial rig
  on the shipped jaw/mouth/brow bones, 86 `.gui` screens/2,383 controls with
  an interactive control-state/screen-stack layer, TLK language and localized
  voice/LIP paths, deterministic
  pazaak/swoop/turret state
  machines, script sound/music/ambient actions, SDL3 output stream.
  CTest **41/41**; 117-module sweep 0/0; VM parity 0; scene parity 0.0000;
  Python 143 tests / 7 skips; install unchanged. See
  `docs/recon/51_AUDIO.md`, `docs/recon/56_LOCALIZATION.md` and
  `docs/SPRINT13_REPORT.md`.
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
- [`docs/SPRINT4_REPORT.md`](docs/SPRINT4_REPORT.md) - Sprint 4 (semantic trace and KotOR profile) exit report
- [`docs/SPRINT4.md`](docs/SPRINT4.md) - Sprint 4 plan: gate S4, lanes, non-goals
- [`docs/SPRINT5_REPORT.md`](docs/SPRINT5_REPORT.md) - Sprint 5 (playable P4 slice) exit report
- [`docs/SPRINT5.md`](docs/SPRINT5.md) - Sprint 5 plan: gate S5, lanes, non-goals
- [`docs/SPRINT6_REPORT.md`](docs/SPRINT6_REPORT.md) - Sprint 6 (P5 script runtime and systems core) exit report
- [`docs/SPRINT6.md`](docs/SPRINT6.md) - Sprint 6 plan: gate S6, lanes, non-goals
- [`docs/SPRINT7_REPORT.md`](docs/SPRINT7_REPORT.md) - Sprint 7 (P5 systems parity II) exit report
- [`docs/SPRINT7.md`](docs/SPRINT7.md) - Sprint 7 plan: gate S7, lanes, non-goals
- [`docs/SPRINT8_REPORT.md`](docs/SPRINT8_REPORT.md) - Sprint 8 (open-work closeout) exit report
- [`docs/SPRINT8.md`](docs/SPRINT8.md) - Sprint 8 plan: gate S8, lanes, non-goals
- [`docs/SPRINT8_5_REPORT.md`](docs/SPRINT8_5_REPORT.md) - Sprint 8.5 (residual systems) exit report
- [`docs/SPRINT8_5.md`](docs/SPRINT8_5.md) - Sprint 8.5 plan: gate S8.5, lanes, non-goals
- [`docs/SPRINT9_REPORT.md`](docs/SPRINT9_REPORT.md) - Sprint 9 (original saves and profiles) exit report
- [`docs/SPRINT9.md`](docs/SPRINT9.md) - Sprint 9 plan: gate S9, lanes, non-goals
- [`docs/SPRINT10_REPORT.md`](docs/SPRINT10_REPORT.md) - Sprint 10 (VM/script closure) exit report
- [`docs/SPRINT10.md`](docs/SPRINT10.md) - Sprint 10 plan: gate S10, lanes, non-goals
- [`docs/SPRINT11_REPORT.md`](docs/SPRINT11_REPORT.md) - Sprint 11 (combat/Force/alignment) exit report
- [`docs/SPRINT11.md`](docs/SPRINT11.md) - Sprint 11 plan: gate S11, lanes, non-goals
- [`docs/SPRINT13_REPORT.md`](docs/SPRINT13_REPORT.md) - Sprint 13 (audio/media/GUI/minigames) exit report
- [`docs/SPRINT13.md`](docs/SPRINT13.md) - Sprint 13 plan: gate S13, lanes, non-goals
- [`docs/SPRINT14.md`](docs/SPRINT14.md) - Sprint 14 plan: mods, KotOR 2 and remaining parity
- [`docs/recon/56_LOCALIZATION.md`](docs/recon/56_LOCALIZATION.md) - TLK language and localized voice/LIP paths
- [`docs/recon/55_BINK_AUDIO.md`](docs/recon/55_BINK_AUDIO.md) - Bink Audio decode, PCM oracle and movie-clock sync
- [`docs/recon/54_FACIAL_RIG.md`](docs/recon/54_FACIAL_RIG.md) - viseme channels and the shipped head bone rig
- [`docs/recon/53_MINIGAMES.md`](docs/recon/53_MINIGAMES.md) - pazaak, swoop and turret deterministic rules
- [`docs/recon/52_GUI_SCREENS.md`](docs/recon/52_GUI_SCREENS.md) - GUI V3.2 control trees and renderer
- [`docs/recon/51_AUDIO.md`](docs/recon/51_AUDIO.md) - audio streams, mixer, area sound and EAX
- [`docs/SPRINT12_REPORT.md`](docs/SPRINT12_REPORT.md) - Sprint 12 (animation/skinning) exit report
- [`docs/SPRINT12.md`](docs/SPRINT12.md) - Sprint 12 plan: gate S12, lanes, non-goals
- [`docs/recon/50_ANIMATION_SKINNING.md`](docs/recon/50_ANIMATION_SKINNING.md) - MDL clips, controllers, skinning and playback
- [`docs/recon/49_COMBAT_FORCE.md`](docs/recon/49_COMBAT_FORCE.md) - Force powers, feats, saves and alignment
- [`docs/recon/48_FUNCTORS.md`](docs/recon/48_FUNCTORS.md) - delayed/assigned closures and OBJECT_SELF
- [`docs/recon/47_ORIGINAL_SAVES.md`](docs/recon/47_ORIGINAL_SAVES.md) - original save read/write and validation
- [`docs/recon/46_RESIDUAL_SYSTEMS.md`](docs/recon/46_RESIDUAL_SYSTEMS.md) - loot, companion combat, merchant depth
- [`docs/recon/45_CLOSEOUT.md`](docs/recon/45_CLOSEOUT.md) - 117-module sweep, encounter reset, formation, feat selection
- [`docs/recon/44_SYSTEMS_UI.md`](docs/recon/44_SYSTEMS_UI.md) - UTM/UTI stores, UTE encounters, companions and panels
- [`docs/recon/43_SCRIPT_AND_RULES.md`](docs/recon/43_SCRIPT_AND_RULES.md) - native NCS VM parity, hooks, dialogue conditions, 2DA rules, save state
- [`docs/recon/41_SEMANTIC_TRACE.md`](docs/recon/41_SEMANTIC_TRACE.md) - live KotOR trace, hook map, tool fixes and the bounded diff
- [`docs/recon/30_UNUSED_ACTIONS.md`](docs/recon/30_UNUSED_ACTIONS.md) - 306 corpus-unused action conformance
- [`docs/recon/31_COMPILER_CODEGEN.md`](docs/recon/31_COMPILER_CODEGEN.md) - 228 compiler-limited recompile reconciliation
- [`docs/recon/32_LIVE_DIFFERENTIAL.md`](docs/recon/32_LIVE_DIFFERENTIAL.md) - live ExecuteCode/action-dispatch and RunCommand differential
- [`docs/recon/33_HISTORICAL_HEADERS.md`](docs/recon/33_HISTORICAL_HEADERS.md) - source-level historical prototype headers (24 cases)
- [`docs/recon/34_ACTION_BEHAVIOR.md`](docs/recon/34_ACTION_BEHAVIOR.md) - behavioral conformance for the 306 unused actions
- [`docs/recon/35_FULL_RECOMPILE.md`](docs/recon/35_FULL_RECOMPILE.md) - full 8,666-script source recompile ledger
- [`docs/recon/36_STRUCT_RECOVERY.md`](docs/recon/36_STRUCT_RECOVERY.md) - source-level `tLastRound` struct recovery (201 files)
- [`docs/recon/37_RETURN_CLEANUP.md`](docs/recon/37_RETURN_CLEANUP.md) - duplicate trailing-return cleanup and 12 historical prototypes (201/201 source-level)
- [`docs/recon/38_BODY_ORDER.md`](docs/recon/38_BODY_ORDER.md) - historical function-address assignment (3 scripts)
- [`docs/recon/39_UNREACHABLE.md`](docs/recon/39_UNREACHABLE.md) - restored unreachable statements (5 scripts, 0 residual)
- [`docs/recon/40_HYDRA_UPGRADE.md`](docs/recon/40_HYDRA_UPGRADE.md) - Hydralysis 0.8.0 / Hydra 12.3-hydra.6 upgrade and Sprint 0-3 impact check
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
