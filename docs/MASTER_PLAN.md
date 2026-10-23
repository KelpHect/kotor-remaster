# KotOR 1:1 Reimplementation — Master Program Plan

Version 0.1 — 2026-10-23
Target artifact: user-owned KotOR 1.0.3.0 build, SHA-256 `0290889e…74e6`.
Companion docs: `recon/01_ARTIFACTS.md`, `recon/02_TECHNICAL_NOTES.md`,
`adr/0001-runtime-and-renderer.md`, `adr/0002-coordinates-and-render-features.md`,
`adr/0003-distribution-and-install-model.md`, `tools/README.md`.

## 1. Thesis

KotOR is a data-driven engine plus content. The executable is a 3.4 MB x86 codebase
(OpenGL, Miles, Bink, win32); the world, dialogue, items, creatures, rules tables, and most
gameplay logic live in BIF/RIM resources, 2DAs, GFF templates, and ~12.6k compiled scripts
(1,774 of which ship with source). A 1:1 rebuild therefore does **not** mean re-authoring the game.
It means:

1. Fully map the executable's behavior and interfaces with evidence.
2. Write a new runtime that consumes the user's original data and reproduces that behavior.
3. Prove parity continuously against the original with differential tests.

Decompiling "the entirety" of the executable is a finite, automatable task. Achieving 1:1
behavior across all 117 areas is a long verification program. This plan separates those two
truths so neither is underestimated.

## 2. Scope and definition of 1:1

In scope:
- All shipped data: base BIFs, 234 module RIMs, 12 global RIMs, `patch.erf`, four texture-pack ERFs
  (11,452 TPC), loose audio/video/LIP files, the 49,265-entry TLK, and saves.
- All 117 module identities, 1,167 DLGs, 1,958 creature templates, 1,784 placeables, all items,
  all 2DAs, all scripts, GUI screens, cutscenes, minigames.
- Engine behavior: rendering, audio, input/camera, resource VFS, object model, script VM and
  772 engine actions, combat, party, inventory, dialogue, journal/quests, transitions, save/load, mods.
- Platforms: Windows first; Linux/Steam Deck via SDL3 + Vulkan; macOS via MoltenVK/GL fallback.
- Save and mod compatibility: original saves load; Override and 2DA/TLK patch workflows work.
- Distribution: a standalone engine + launcher reads a user-owned KotOR install in place; the
  original `swkotor.exe` is not required, the install is not modified, assets are never shipped or
  downloaded, and caches live outside the install. See ADR 0003.

Out of scope for the engine: distributing game assets, DRM circumvention or ownership checks,
re-authoring content, online services (K1 has none), and using GPL prior-art code inside a
differently licensed engine.

Fidelity levels, tracked separately:
- **F0 Data parity** — every resource parses, validates, and round-trips.
- **F1 Mechanical parity** — scripts, actions, state machines, rules, and RNG produce the same outcomes.
- **F2 Presentation parity** — Classic renderer/audio/camera/timing match within documented tolerances.
- **F3 UX parity** — menus, GUI, input, tooltips, accessibility options.
- **F4 Bug parity** — Classic mode reproduces known original bugs by default; optional "fixed" mode.
- **F5 Mod parity** — Override, patcher-style 2DA/TLK merges, widescreen and texture packs.

## 3. Ground truth (verified)

| Fact | Value |
|---|---|
| Executable | PE32 i386, image base `0x400000`, entry `0x6fb38d`, 3.39 MB `.text`, no `.bind`, no Rich header |
| Imports | OpenGL32, GLU32, DInput8, MSS32 (Miles), BinkW32, win32 core |
| Base resources | 25,836 in 26 BIFs (KEY/BIF reconciliation exact) |
| Module resources | 234 RIMs = 117 area pairs, 20,575 entries, 71,421,011 bytes |
| Global/patch/textures | 12 global RIMs (1,936), `patch.erf` (97), four texture ERFs (11,453), ~59,897 resources pre-dedup |
| Strings | `dialog.tlk` V3.0, 49,265 entries |
| Scripts | 1,784 NCS + 1,774 NSS in base; 10,863 NCS in modules; **772 actions, 772 handlers mapped** (774 writes) |
| Binary | 8,616 defined functions / 809,297 instructions; **8,616/8,616 decompiled** (13.5 MB pseudocode); 440 `LAB_` action targets + dispatcher still to define |
| VM | NCS big-endian, 13-byte header, opcodes 0x01–0x2D; table verified; dispatcher `0x0052c0d0` (bound 772, null `-2002`) |
| Key content | 117 ARE/GIT/IFO, 2,832 MDL + 2,832 MDX, 5,601 TGA + 5,615 TXI, 1,554 WOK/DWK/PWK, 1,167 DLG, 209 2DA, 84 GUI, 1,928 WAV, 16 SSF |
| Loose media | 61 Bink movies (579 MB), 119 music streams (191 MB), 970 sound streams (115 MB), 13,799 waves (553 MB), 642 MB texture packs |
| Toolchain | Ghidra 12.1.4 (JDK 25 currently; pin JDK 21), Java 25, Python 3.15, Node 24, Git 2.55, SDL3/Vulkan planned |

Resource type IDs were verified by reading payload magic; see `recon/01_ARTIFACTS.md`.

Provenance incident: the old Ghidra program (SHA-256 `34e6d971…`, `.bind`, entry `0x86d2ed`)
was a different build and is discarded. The fresh project `artifacts/ghidra/kotor.gpr` imported
the current file (loader MD5 `a9f8b94a…`) and completed analysis. All claims must cite the
current hash.

## 4. Strategy

### 4.1 Evidence first
Every RE claim carries: target SHA-256, Ghidra project/version, address, function/body hash,
decompiler-text hash, claim, confidence, and test/evidence link. Unproven statements are written
as open questions, never as facts. These follow the existing RE workflow rules.

### 4.2 Clean-room separation
- **Spec lane** reads decompiler output/runtime and writes behavioral specifications, tables, and
  fixtures in project-owned words. Decompiled text stays in a quarantined evidence store.
- **Implementation lane** writes engine code from specs and tests only. No copied pseudocode,
  identifiers, comments, or structure; no GPL code.
- Reusable ground truth: MIT-licensed Kaitai format specs (OpenKOTOR) with attribution.
  GPL/LGPL tools (xoreos-tools, PyKotor, reone, kotorblender) run as external oracles only.
- Contributor provenance and a taint log are mandatory. Legal review before any public release.

### 4.3 Data-driven runtime
Load the user's data at runtime or through a per-install cache. Never bundle, download, or commit
assets. Preserve original 32-bit integer/float semantics, unknown GFF fields, and resource
precedence (`Override` > module/patch > base archives; exact order to be measured).

### 4.4 Original as oracle
The original executable is the behavioral reference. Capture file I/O, VM action calls, RNG,
combat rounds, save state, OpenGL calls, and screenshots under controlled scenarios. The new
engine must match state and outcomes; rendering matches within tolerance via the Classic backend.

### 4.5 Vertical slices, not big-bang
Ship one complete area end-to-end early (movement, camera, scripted objects, dialogue, combat,
save/load, Classic renderer). Use it to validate every layer before content sweeps.

### 4.6 Automate coverage
Every function, resource, script, area, action, and dialogue gets a ledger entry and a status.
CI rejects regressions in coverage and conformance. No "done" without a passing artifact.

## 5. Program lanes (10 parallel lanes)

| Lane | Owns | Primary outputs | Depends on |
|---|---|---|---|
| L0 Program/Evidence/Integration | Evidence DB, shared schemas, merge policy, coverage dashboard | claim ledger, generated tables, release gates | all |
| L1 Formats & VFS | KEY/BIFF/RIM/ERF/SAV, GFF, 2DA, TLK, SSF, LIP, TPC/TXI, MDL/MDX, LYT/VIS/BWM/PTH, GUI, precedence | parsers, extractors, validators, round-trip suite | L0 |
| L2 Binary RE | Ghidra project, function map, class/vftable recovery, action table, subsystem map | function inventory, specs, engine map | L0 |
| L3 NWScript VM & actions | opcode semantics, dispatch, 772 actions, timers, object model | VM, differential harness, decompiler/oracle | L1, L2 |
| L4 Game rules & simulation | combat, feats/powers, party, inventory, merchants, XP, journal, minigames | deterministic simulation + tests | L2, L3 |
| L5 Areas/navigation/cutscenes | coordinate/transform contract, module load, LYT/VIS/WOK/PTH, collision, AI nav, triggers, cameras, map-compile validation | area runtime + pathfinding + clean map reports | L1, L2 |
| L6 Renderer/RHI | Vulkan 1.4 primary, GL 4.5 reference, Classic/Remaster paths, baked/dynamic shadows, optional RT/FSR, shaders | renderer, frame capture, parity | L1, L5 |
| L7 Audio/video/UI | Miles/EAX replacement, Bink decode, LIP sync, movie playback, `.gui` UI, fonts | media layer + UI system | L1 |
| L8 Engine runtime & tools | core loop, ECS/object store, jobs, async I/O, save system, debug tools | engine skeleton, inspectors | L0 |
| L9 Verification/harness/CI | original-game harness, replay, state diffs, screenshot/audio diffs, coverage CI | harness, golden corpus, CI | L0, L2 |

Lane rules: one integrator (L0) owns shared generated files; lanes use separate git worktrees;
one shared game process, one Ghidra project, and one evidence-DB writer at a time; a skeptic
re-derives high-impact claims (action table, VM semantics, save layout) before acceptance.

## 6. Phases and gates

### P0 — Foundation (week 1)
- Freeze artifact manifest; pin JDK 21; validate fresh Ghidra project hash.
- Export all functions; classify externals/thunks/CRT/middleware vs engine.
- Stand up evidence DB (SQLite) and repo skeleton.
- Implement install detection/validation and the external cache profile (data set, version,
  language, hashes; no executable required; read-only install).
- Run the original once under a file/registry monitor; capture baseline settings and saves.
- Gate G0: reproducible build of tools; analysis hash match; inventory reconciled; function
  inventory exported with hashes; a clean install validates and caches without touching it.

### P1 — Data completeness (weeks 2–6)
- Implement all format parsers (Kaitai MIT specs as a starting point); extract every resource.
- Round-trip GFF/ERF/RIM/2DA/TLK; validate TLK strings and 2DA row/column semantics.
- Implement the save parser/writer (SAVEGAME.sav, nested module saves, sidecars) and a VFS
  precedence harness with discriminating assets; measure TexturePack quality selection.
- Run the coordinate probe; freeze the source→canonical transform contract; build landmark fixtures
  (objects, doors, waypoints, cameras) and a map compiler that hard-fails on bad winding, bounds,
  walkmesh/PTH connectivity, lightmap UVs, or misplaced spawn/transition anchors.
- Build per-install cache with content hashes; first geometry/texture/UI viewer.
- Deduplicate scripts by content hash; pair NSS with NCS; build script corpus.
- Gate G1: 100% of base + module resources parsed with zero unhandled types; round-trip suite green;
  resource coverage ledger complete.

### P2 — Binary map (weeks 2–10, parallel with P1)
- Bulk decompile every function; export pseudocode + hashes.
- Recover vftables, constructors/destructors, class layouts, string/constant xrefs.
- Identify resource manager, VM/dispatch table, action functions, renderer pipeline, audio,
  input/camera, object/save system, combat, AI, module loader.
- Build the engine map: subsystem, classes, functions, threads, state ownership, interfaces.
- Gate G2: 100% of engine-relevant functions classified; action table enumerated and matched
  (**772/772**); dispatcher and all action-handler targets defined/decompiled; every engine action
  has a spec stub; no unexplained entry points.

### P3 — VM conformance (weeks 6–14)
- Implement NCS decode/execute: big-endian bytecode, 4-byte typed stack, SP/BP, opcodes 0x01–0x2D,
  `SAVEBP`/`RESTOREBP`, `STORE_STATE`, `DESTRUCT`, object validity, recursion limit 8.
- K1 action table is **fully mapped: 772/772**. `0x0054c960` allocates `0xC10` (772 × 4) and writes
  677 slots, then tail-jumps to `0x005cd2c0` (97 more): 774 writes, ids 156/709 duplicated. The
  earlier 757 count was a parser artifact (15 decimal offsets + slot 0). The dispatcher
  `0x0052c0d0` is vtable `0x7450e4` slot +8 (pointer verified in the file); it checks id < 772,
  rejects null with `-2002`, and calls `handler(id, arg)`. ExecuteCode's ACTION path targets this
  service. There is no second table or base implementer.
- Coverage follow-up: 565 unique handler targets (125 `FUN_`, 440 `LAB_`). Define the dispatcher and
  audit/define all 440 `LAB_` targets, then re-export pseudocode. The 8,616-function count is the
  initial pass, not final code coverage.
- Implement/mediate all 772 actions; start with logging stubs, then functional groups.
- Decompile the corpus with DeNCS (MIT) and PyKotor/xoreos as independent oracles, diff outputs,
  recompile where possible, and publish per-file exact/normalized/failed status.
- Differential harness: hook `ExecuteCode`/`RunCommand`, run identical synthetic states in the
  original and new VM, compare IP/opcode/SP/BP/stack types/values/action args/returns/side effects.
  Run all unique shipped scripts.
- Gate G3: all unique shipped NCS execute; zero unimplemented actions in the corpus; synthetic
  differential suite green; object/timer semantics documented.

### P4 — Vertical slice (weeks 10–20)
- Choose one complete module pair (e.g. Endar Spire or a Taris apartment) with manageable scope.
- Implement resource VFS, area load, player movement/camera, collision/walkmesh, doors, triggers,
  placeables, NPCs, dialogue, basic combat, module transition, save/load, minimal GUI.
- Ship Classic renderer on Vulkan 1.4 and GL 4.5; debug inspectors.
- Gate G4: 30-minute scripted playthrough reproduces expected events; save round-trips; Classic
  captures pass tolerance; both backends render the same scene. Coordinate/map gate: landmark
  placements and headings match, walkmesh/path tests pass, map validation reports are clean, and
  Vulkan projection/viewport handling is proven against a known screen landmark.

### P5 — Systems parity (months 5–10)
- Full combat rounds, feats/Force powers, party, equipment/inventory, merchants, XP/levels,
  alignment, journal/quests, cutscene cameras, encounters, minigames (pazaak, swoop, turret),
  audio/EAX approximation, movies/lip sync, full `.gui` screens.
- Use the ownership split in `recon/02`: apply data-owned 2DA/rules, implement engine-side d20
  combat/XP/pricing/transitions, and run script-owned effects through the VM. Test 30-point buy,
  caps, XP thresholds, crit/save/deathblow, party 80% XP, difficulty damage scaling, merchant
  rounding, DLG branch gating, encounters, and all three minigames.
- Bug-parity matrix starts with the documented glitches/exploits (Anywhere Menu Glitch, Displaced
  Load Zone, Spawn Warps, Hotshot, The Routine, Fake Level Up, item duping); Classic reproduces,
  fixed mode does not.
- Save compatibility: load original saves, save from the new engine, and round-trip back; preserve
  IFO/GIT/PARTYTABLE/GLOBALVARS semantics, `UseTemplates=0`, and TSLPatcher appends. Cover autosave,
  quicksave, module transitions, `.rsv` shadowing, and corrupt `EventQueue` cases.
- Mod support: Override, TSLPatcher-style 2DA/TLK merges, texture packs, widescreen assumptions.
- Gate G5: system test matrix green; original saves load; representative mods work; all 117 areas
  load; all DLG trees open and play.

### P6 — Content parity (months 10–24)
- Sweep every module, quest, dialogue, cutscene, item, creature, encounter, and localization path.
- Differential playthroughs; save/load matrix; performance; platform ports.
- Bug-parity mode vs fixed mode; accessibility; controller support.
- Gate G6: full-game checklist complete; no P0/P1 parity bugs; performance budgets met; legal
  compliance review passed; release candidate.

### P7 — Release and enhancements (24+ months)
- Installer/launcher that points at a user-owned install, validates the data set, builds the cache
  outside the install, and never modifies or redistributes game files (ADR 0003).
- Remaster path, staged: (1) high-res textures, HFR/widescreen, MSAA/anisotropy, SSAO/bloom;
  (2) re-baked static shadows/AO and dynamic shadow maps; (3) optional RT shadows/reflections on
  capable hardware, with raster fallback; (4) FSR-class upscaling with depth/motion vectors.
  Classic remains the 1:1 oracle and is selectable at runtime.
- Documentation, modding APIs, platform builds.

Estimates assume 6–12 engineers plus the 10-agent RE swarm; a solo effort scales to multiple years.

## 7. Architecture summary

- **Runtime:** original C++23, SDL3 platform/input/audio, custom object/simulation model, fixed tick.
- **RHI:** Vulkan 1.4 primary, core 1.4 features with feature detection and 1.3 floor; OpenGL 4.5
  reference/fallback; D3D12/Metal later behind the same interface. See ADR 0001.
- **Render paths:** Classic (2003 fixed-function emulation, oracle) and Remaster (optional modern
  effects), sharing scene data; no GPU API in gameplay code.
- **Data:** VFS with original precedence; schema-driven GFF; typed 2DA/TLK; original saves.
- **Scripts:** independent NCS VM with an action registry generated from the verified action table;
  fixed-step timers; explicit RNG streams.
- **Determinism:** int32/float32 semantics, ordered event queue, stable IDs, deterministic path
  tie-breaks, no engine physics/navmesh, no fast-math.
- **Tooling:** Ghidra exports → SQLite evidence DB → specs → engine tests; asset cache per install;
  ImGui inspectors; headless replay and diffing.

### 7.1 Asset pipeline

- Per-install **offline cache** for base data; **runtime overlay** for `Override/`, MOD/SAV, and mod
  files with on-demand conversion. Never mutate the install; never commit assets.
- Planned native tools (`tools/`): `index` (VFS + manifest), `data2json` (GFF/2DA/TLK/LIP),
  `tpc2ktx` (TPC/TXI → PNG/KTX2 + material JSON), `mdl2gltf` (MDL/MDX → glTF + KotOR sidecar),
  `bwm2nav`, `cache`, `validate`. Python for orchestration, Rust for hot converters.
- v0 ground truth: run `kotorblender` headless and PyKotor as external oracles, then replace with
  first-party converters to avoid license coupling.
- TPC: 128-byte header, alpha-test float, gray/RGB(A)/DXT1/3/5, mips, cubemap faces, TXI appended.
  Some WAV SFX carry a ~470-byte prefix before `RIFF`; always probe signatures.
- MDL/MDX: no magic; 12-byte header; offsets exclude the header; 30 FPS animations, 4-weight
  skinning, hooks, lights, emitters, supermodel inheritance.
- Coordinates: the source is reported Z-up right-handed (verify with the coordinate probe and
  landmark fixtures; see ADR 0002). The provisional mapping to canonical Y-up right-handed is
  `(x, y, z) → (x, z, -y)` with conjugated quaternions; never swap axes alone (winding flips), and
  confirm the mapping before freezing the cache format.
- Preserve original winding, normals, UV orientation, vertex colors (BGRA), alpha test/blend, and
  lightmap/vertex-lighting gamma behavior for the Classic path.
- BWM: triangles carry surface IDs, adjacency, perimeter, AABB; do not retriangulate. Build nav from
  `surfaces.2da` and use PTH + walkmesh line-of-sight. PTH uses the shipped key `Path_Conections`.
- Texture packs A/B/C have identical resrefs (3,294 each); measure the selection rule before caching.

### 7.2 Dynamic analysis and harness

- Current build: no `.bind`, no `steam_api` import, fixed base `0x400000`, RELOCS_STRIPPED, not LAA.
  Address tables are keyed to the exact SHA-256 plus prologue self-tests; never reuse offsets across builds.
- Frida 17.24.3 was verified for ia32 attach/interception on this host; MinHook/Detours build a 32-bit
  harness DLL for stable low-rate hooks; x32dbg for live state. RenderDoc is unsuitable for legacy GL;
  use a 32-bit apitrace build and Mesa llvmpipe for reference captures.
- Hook the single NWScript action dispatcher rather than 772 functions; hook the resource manager,
  input, combat round, save/load, and RNG (MSVC LCG at RVA ~`0x2FC451` in this build, verify by pattern).
- Serialize one instrumented game process per suite; control via a named pipe (`load_save`,
  `step_frames`, `input`, `snapshot`, `quit`); log JSONL with frame/round IDs and hashes.
- Scenario replay is realistic from fixed saves + fixed config + recorded RNG + virtual clock.
  Bit-exact full-game replay is not; compare semantic state, saves, and perceptual captures.
- Never patch or remove DRM. If a wrapped build is ever analyzed, observe only a legitimately
  running process and do not distribute circumvention tooling.

### 7.3 Coordinates, map compilation, render features

- Canonical space: right-handed Y-up meters; convert once at the importer, never in gameplay or
  renderer code. Source conventions (Z-up, handedness, units, quaternion order, matrix layout) are
  provisional until the coordinate probe and landmark fixtures verify them. See ADR 0002.
- Decode and validate: GFF positions/orientations, MDL pivots/bind poses/supermodels/animations,
  BWM/WOK/DWK/PWK winding and surface IDs, LYT/VIS/PTH spaces, door/transition anchors, cameras,
  UI coordinates. Vulkan clip-space Y/depth/winding adjustments live in the RHI/projection layer.
- Map compiler: produces scene geometry, visibility, walkmesh, path graph, lightmaps, spawns, and
  metadata; hard-fails on inverted winding, bounds/connectivity problems, bad lightmap UVs, or
  misplaced anchors. Debug overlays and original captures are the comparison tools.
- Classic mode: original fixed-function pipeline, lightmaps, stencil shadows, alpha/fog, GUI.
  Remaster mode: re-baked static shadows/AO, dynamic shadow maps, optional RT shadows/reflections,
  high-res textures, HDR/post, FSR/DLSS/XeSS-class upscaling. It shares data/simulation and never
  affects saves or outcomes.
- Vulkan 1.4 is the API; RT needs `VK_KHR_ray_query`/`ray_tracing_pipeline` and capable hardware;
  FSR is a pinned AMD SDK integration with depth/motion vectors. Raster fallback is mandatory.

### 7.4 Save/load and VFS

- `SAVEGAME.sav` is an ERF (MOD V1.0) containing one nested `<module>.sav` ERF (type 2057) per
  visited module, plus REPUTE, AVAILNPCn, INVENTORY. Each module save holds Module.ifo, Module.are,
  Module.git. Sidecars are savenfo.res, PARTYTABLE.res, GLOBALVARS.res, Screen.tga; autosaves omit
  the screenshot and add pifo.ifo. All are GFF V3.2.
- State map: IFO entry/clock/player list; GIT live objects and cameras; ARE static data; PARTYTABLE
  roster/gold/journal; GLOBALVARS positional arrays; equipment/inventory in snapshots. The load trap
  is `UseTemplates=0`: saved GIT objects are flattened and must not be re-templated.
- VFS precedence is five passes, first match wins: directories (Override, stream audio, movies);
  ERFs tagged 1 (patch.erf); RIMs (module _a/_adx/_s, global, menu); ERFs tagged 2 (module .mod,
  saved module state, lips, players, TexturePacks); then chitin.key/BIFs. Last-added table is
  searched first; first duplicate `(resref,type)` wins; resrefs are 16-byte ASCII, lowercased.
- A module `.mod` loses to every RIM including `global.rim`; a save only shadows resources it
  actually stores. Texture-pack quality mapping (tpa/tpb/tpc) is still to measure.
- Save/load anchors: `CSWSModule::SaveModuleStart/Finish`, `StallEventSaveGame`, `DoPCAutosave`,
  `CERFFile::ImportFiles`; load via `LASTMODULE`, `CopyGameToFutureGame`, `LoadModuleStart`.
  Hazards: external `<module>.rsv` shadowing, corrupt `EventQueue`, mod removal dangling resrefs.
- TSLPatcher-compatible mods append 2DA rows/TLK strings and preserve indices; whole-file
  replacements break old saves. Test Override vs RIM vs patch vs TexturePack ordering, duplicate
  keys, case variants, two stacked patcher mods, and a synthetic save round-trip.

## 8. Decompilation program (the "entirety")

### 8.1 Coverage target
- Analyze all code in `.text` (3.39 MB), including static CRT, middleware call sites, and engine code.
- Initial metadata inventory: 8,616 defined functions / 809,297 instructions / 2.71 MB of bodies.
  The action table adds 440 `LAB_` handler targets and dispatcher `0x0052c0d0` outside this count;
  define/decompile them before declaring full coverage.
- Export every function with pseudocode; classify:
  `engine / CRT / middleware / thunk / external / data / unknown`.
- "Complete" means every function is classified and every engine function has a spec or an explicit
  exclusion rationale; not every line is hand-written into prose.
- Track decompiled coverage, named coverage, spec coverage, implementation coverage, and test
  coverage separately.

### 8.2 Toolchain
- Ghidra 12.1.4 project (upgrade to 12.2 after verification), JDK 21, PyGhidra, Function ID/BSim,
  version tracking across owned builds.
- Custom exporter (`tools/ghidra/ExportFunctionInventory.java`) emits JSONL with body/pseudocode hashes.
- Evidence queries through REA/Ghidra MCP; bulk work through headless scripts; Frida/x32dbg for runtime.
- Prior art as a test index only (reone's subsystem and script-routine inventory); never as source.

### 8.3 Priority order
1. Action table + VM interpreter (unlocks all scripts).
2. Resource manager/VFS, GFF/2DA/TLK, module/area load.
3. Object model, save/load, events/timers.
4. Renderer pipeline and camera.
5. Combat/AI/party/inventory/dialogue.
6. Audio/UI/movies/minigames.
7. CRT/middleware identification and exclusion.

### 8.4 Dynamic evidence
- Launch the user's 1.0.3.0 build; attach Frida (ia32 confirmed) for discovery; native MinHook DLL
  for stable hooks; x32dbg for state inspection; apitrace/Mesa for GL.
- Hook the single NWScript action dispatcher, resource manager, RNG, combat round, save/load, input.
- Record JSONL with frame/round IDs; capture file traces, saves, screenshots, GL streams.
- Never patch or remove DRM; if a wrapped build is ever used, analyze only a legitimately running
  process and avoid circumvention. Current build has no wrapper.

## 9. Verification program

| Layer | Method | Pass criteria |
|---|---|---|
| Formats | Parse every resource; round-trip; cross-check with independent oracles | 100% parsed; byte/semantic round-trip; zero unhandled types |
| VFS | Discriminating duplicate resources across Override/RIM/ERF/BIF/texture packs; hook the resolver | Observed precedence matches the five-pass model; texture quality mapping known |
| Install | Clean/partial/modded/localized installs; validation and cache invalidation; read-only check | Clear diagnostics; cache keyed by install; original folder unchanged |
| Transforms/map compile | Landmark fixtures, quaternion/pivot/bind-pose tests, walkmesh/path tests, projection tests, map validator | Exact placements/headings; clean map reports; Vulkan projection proven |
| Render features | Classic vs Remaster captures; RT/FSR on/off; hardware fallback | Classic parity; Remaster no gameplay divergence; raster fallback works |
| Functions | Ledger classification; skeptic review | 100% classified; key claims re-derived |
| VM | Opcode/action traces vs original on synthetic + shipped scripts | Exact semantic trace on corpus; zero unimplemented actions |
| Simulation | Scenario replay, state hashes, save diffs, RNG sequences | Same outcomes/state within documented tolerance |
| Rendering | Fixed camera/tick captures; SSIM/pixel diff; Vulkan vs GL parity | Thresholds per scene class; no gameplay divergence |
| Audio/video | Stream hashes, sync markers, event logs | Correct clip, timing, volume/3D placement |
| Saves | Original→new→original round-trip; field-level diffs | Loads, preserves progress, no corruption |
| Mods | Curated corpus (Override, 2DA/TLK patches, textures) | No regressions vs original behavior |
| Performance | Frame/load budgets on reference hardware | Stable frame time; faster loads than original |

Golden corpus: all scripts, all 117 areas, all DLGs, representative combat encounters, every menu,
every item/creature template, all minigames, all save types, localization.

## 10. 10-agent operating model

- **Integrator (L0)** owns shared generated files: action table, type map, address index, schemas.
- **Skeptic** independently re-derives the top 20 critical claims (VM dispatch, action count, save
  layout, RNG, VFS order, renderer state) and caps review rounds.
- **Lane agents** work in isolated worktrees; no lane edits another's generated outputs.
- **Resource locks:** one Ghidra project (read-only export passes), one instrumented game process,
  one evidence-DB writer endpoint.
- **Stop rule:** three identical failed attempts → recon doc + escalate; never thrash.
- **Deliverable per lane per week:** evidence records, spec update, tests, coverage delta, blockers.
- **Merge:** small PRs with provenance; generated files regenerated, not merged by hand.

Suggested first 10 assignments:
1. Format parser core (KEY/BIFF/RIM/ERF) + tests.
2. GFF schema + typed object model.
3. 2DA/TLK/SSF/LIP parsers + mod merge semantics.
4. MDL/MDX + LYT/VIS/BWM/PTH parser and asset cache.
5. TPC/TXI/audio/movie pipeline.
6. Ghidra function classification + action-table hunt.
7. NWScript opcode/VM differential harness.
8. Area/object/save-system RE.
9. Renderer/camera/material RE + Classic shader prototypes.
10. Dynamic harness (Frida/MinHook, file/VM/RNG traces) + CI evidence.

## 11. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Scope: 117 areas, 12.6k scripts, full rules | Years, burnout | Data-driven reuse; vertical slice; coverage gates; no content re-authoring |
| Legal/IP/EULA | Takedown, liability | Clean-room, no assets, no DRM circumvention, counsel, neutral branding |
| GPL contamination | License infection | Tools as external oracles; MIT Kaitai specs; taint log; spec/impl split |
| No PDB/RTTI, MSVC C++ | Slow RE | Vftable/ctor patterns, FID/BSim, dynamic traces, prior-art index, skeptic review |
| Timing/RNG/float fidelity | Behavior drift | Fixed clock, recorded RNG, int/float discipline, state-hash tests, bug-parity mode |
| Renderer fidelity | Visual mismatch | Classic compatibility path, fixed captures, GL reference backend |
| Coordinate/clip-space mismatch | Inverted/misplaced maps, bad cameras | ADR 0002 single conversion boundary, landmark fixtures, map validator, Vulkan projection tests |
| RT/FSR hardware/SDK variance | Feature gaps, visual divergence | Capability tiers, raster fallback, pinned FSR SDK, optional RT with no gameplay dependency |
| Install/version drift | Broken first-run, wrong cache | Data-set validation, hashes, per-install profiles, warnings for mods/unknown versions (ADR 0003) |
| Ownership/legal posture | C&D, storefront rejection | No assets, no DRM/ownership checks, neutral branding, counsel; runtime requires user data |
| Middleware (Miles/Bink) | Audio/video gaps | Replace APIs, FFmpeg Bink, EFX/EAX approximation, golden audio tests |
| Save/mod compatibility | User data loss | Early save parser/writer, round-trip matrix, mod corpus |
| Toolchain drift | Unreproducible results | Pin hashes/versions, JDK 21, Ghidra upgrade plan, evidence DB |
| Vulkan complexity | Slow renderer work | RHI abstraction, GL reference, first slice on both, validation layers/CI |

## 12. Immediate Sprint 0 checklist

- [x] Hash and fingerprint the current executable; discard stale Ghidra program.
- [x] Re-import current executable into a fresh Ghidra project; analysis succeeded.
- [x] Build read-only KEY/BIFF/RIM inventory and signature probe; verify type table.
- [x] Export full function inventory JSONL: 8,616 functions, 809,297 instructions.
- [x] Export pseudocode for all functions: 8,616/8,616, 0 errors, 13.5 MB.
- [x] Extract `nwscript.nss`, build the 772-action table, map all 772 handlers (774 writes).
- [x] Pin Temurin JDK 21.0.12.1+1 at `tools/jdk21` (archive SHA-256 recorded).
- [x] Stand up SQLite evidence DB and coverage view (`tools/evidence/ledger.py`).
- [x] Validate TLK V3.0 parser against `dialog.tlk` (49,265 entries, 3.42M chars).
- [ ] Define dispatcher `0x0052c0d0`; audit/define the 440 `LAB_` action-handler targets; re-export.
- [ ] Freeze full artifact manifest (exe, key, tlk, erf, BIF headers, module hashes).
- [ ] Implement GFF/2DA parsers from MIT Kaitai specs; validate against every resource.
- [ ] Decompile every NCS with DeNCS/PyKotor/xoreos; publish per-file status.
- [ ] Run the original under Frida; capture file I/O, RNG, combat and save traces for one area.
- [ ] Implement the save parser/writer and VFS precedence fixtures; measure texture-pack selection.
- [ ] Scaffold the C++23/SDL3 runtime with Vulkan 1.4 + GL 4.5 backends and ImGui.
- [ ] Render first area geometry from original data; screenshot and compare.
- [ ] Create lane worktrees and evidence tasks; start L1/L2/L9 in parallel.

## 13. Repository layout (proposed)

```
engine/
  src/core/        loop, jobs, time, RNG, events
  src/vfs/         KEY/BIF/RIM/ERF/SAV, precedence
  src/formats/     GFF, 2DA, TLK, SSF, LIP, MDL/MDX, TPC, LYT/VIS/BWM/PTH, GUI
  src/script/      NCS VM, action registry, compiler bridge
  src/sim/         objects, combat, party, inventory, dialogue, quests
  src/scene/       area graph, streaming, cameras, cutscenes
  src/render/      RHI, Vulkan 1.4, GL 4.5, classic/remaster passes
  src/audio/       mixer, 3D audio, EAX approximation, movies
  src/ui/          GUI runtime, fonts, input
  src/save/        original save compatibility
  src/platform/    SDL3, window, input, filesystem
  src/tools/       inspectors, replay, cache builder
tools/             inventory, ghidra, parsers, asset pipeline, harness
tests/             synthetic fixtures, conformance suites (no game data)
docs/              master plan, recon, ADRs, format/VM/engine maps, coverage
artifacts/         gitignored: Ghidra project, exports, caches, capture evidence
```

## 14. Open decisions

1. ~~Engine core sign-off~~ — **decided: custom C++23 + SDL3 + Vulkan 1.4**, OpenGL 4.5 reference backend.
2. Default fidelity: bug-parity Classic mode vs fixed-mode default (recommend Classic default,
   fixes opt-in).
3. Release posture: private preservation build vs public clean-room release (affects legal gates).
4. Mod compatibility scope: Override + 2DA/TLK patching first; TSLPatcher parity later.
5. Localization scope: English first, then all shipped languages.
6. Remaster features priority: high-res textures/HFR first; re-baked/dynamic shadows next; optional
   RT and FSR-class upscaling after. Classic remains selectable.
7. RT/FSR hardware floor: which GPU/driver tiers are supported, and whether RT is opt-in only
   (recommended: opt-in with raster fallback).
8. Supported installs: KotOR 1.0.3.0 English first (Steam/GOG data layouts); later languages and
   versions. Default cache location and whether saves write to the original folder or a profile
   (recommend external cache and explicit save location).
9. Ownership verification: **none now**. A future distribution variant could add an optional
   Steamworks entitlement check in a separate launcher/policy layer; it requires a Steam dev
   account, has offline/other-store implications, and is explicitly out of scope until a product
   decision. See ADR 0003.

## 15. References and prior art

- Kaitai format specs (MIT): https://github.com/OpenKOTOR/bioware-kaitai-formats
- PyKotor (LGPL, tooling/oracle): https://github.com/OpenKOTOR/PyKotor
- reone (GPL, prior art / test index): https://github.com/seedhartha/reone
- KotOR.js (GPL, prior art): https://github.com/KobaltBlu/KotOR.js
- xoreos/xoreos-tools (GPL, oracles): https://xoreos.org/
- kotorblender (GPL, asset authoring): https://github.com/seedhartha/kotorblender
- Ghidra: https://github.com/NationalSecurityAgency/ghidra
- Frida: https://frida.re/ ; MinHook: https://github.com/TsudaKageyu/minhook
- SDL3: https://www.libsdl.org/ ; Vulkan: https://www.vulkan.org/
- FFmpeg Bink decoder: https://github.com/FFmpeg/FFmpeg
