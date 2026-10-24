# P0 foundation report — complete (strict closure)

Status: **P0 foundation complete as of 2026-10-24.** All six P0 items and Gate G0 are met with
reviewed evidence. Under the requested stricter standard, every listed observation was either
closed with direct evidence or moved to the P1/P4 handoff with its boundary stated; none remains
as an unresolved P0 measurement. This does not claim a playable rebuilt engine (P4/P5) or semantic
equivalence of all 10,849 functions.

## Historical foundation checks

| # | P0 item | Result | Evidence |
|---:|---|---|---|
| 1 | Freeze manifest; pin JDK 21; validate fresh Ghidra project hash | **met** — 15,380 files / 3,602,889,094 B; JDK 21.0.12.1+1; target SHA-256 `0290889e…` | `docs/recon/06`, `tools/README.md` |
| 2 | Export all functions; classify externals/thunks/CRT/middleware vs engine | **met with export recorded** — exhaustive 11,199 rows: 10,302 engine, 441 CRT, 350 external, 76 thunks, 30 helpers, 0 unknown; 0 unexplained `.text` bytes; 10,849/10,849 internal functions decompiled, 0 failures | `docs/recon/14` |
| 3 | Evidence DB + repo skeleton | **met** — SQLite ledger (43 cited claims; 10,849/10,849 functions, 772/772 actions), public repo, lane artifacts | `tools/evidence/ledger.py`; repo |
| 4 | Install detection/validation + external cache profile | **met** — validator, deterministic profile (`6bf42d3c…`), 14 tests, output-refusal guard | `docs/recon/13` |
| 5 | Original run under file/registry monitor; baseline settings/saves | **met** — 332 read-only install opens/run, only profile `swkotor.ini` written, registry exports byte-identical at baseline; later acceptance runs verified save load/write/reload | `docs/recon/08`; lane 06 `cont/`; `docs/recon/10` |
| 6 | Gate G0: reproducible tools, hash match, inventory reconciled, clean-install validate/cache | **met** — profile byte-identical across runs; manifest 0 missing/extra/size diffs; validator + verify pass without writes | `docs/recon/13`; `artifacts/lanes/p0-install-cache/out/` |

## Gate G0 detail

* **Reproducible tool output:** two cache-profile builds were byte-identical
  (`ca86e259…5bbbafe`); profile hash `6bf42d3c…348fd1`; install snapshot before/after identical.
* **Analysis hash match:** Ghidra target SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`;
  baseline project never modified by classification.
* **Inventory reconciled:** 15,380 files / 3,602,889,094 B, 0 missing, 0 extra, 0 size diffs;
  dataset subset 279 files / 2,113,227,942 B, 0 hash/size disagreements.
* **Function inventory:** v4 exhaustive inventory 10,849 internal + 350 external, all high
  confidence; 0 unexplained `.text` bytes. The v4 export contains 10,849/10,849 decompiled
  functions (`functions-v4.jsonl`, SHA-256
  `8c106dc051254aa794e46be47ebf29378378781bcd71953f1b3fcd5b7623578d`), 0
  failed/missing/extra/duplicate or exact-range-overlapping bodies; ledger coverage is
  10,849/10,849 functions and 772/772 actions.
* **Clean install:** `install_validate.py` returns VALID with 0 errors/warnings; `cache_profile.py
  verify` matches 279/279; both refuse to write inside the install.

## Dynamic baseline (P0 item 5)

Three off-screen runs reached `MAINMENU.rim`:

* 436 relevant file opens per run; 332 resolve into the install and are all `GENERIC_READ` /
  `OPEN_EXISTING`.
* The only write-intent opens are 14 `.\swkotor.ini` opens in the profile; the profile ini is
  rewritten with identical content. The install is unchanged.
* Registry exports before/after are byte-identical (`34f1f526…`). The only KotOR key is
  `HKLM\SOFTWARE\WOW6432Node\BioWare\SW\KotOR` (`path`, `flavour=Retail`); no HKCU/LucasArts key.
  18 write attempts per run were blocked (AMD + DirectInput), none KotOR-related.
* No `saves/` or `modules/*` resource is opened at baseline; the reference install has no save
  folder. Later acceptance runs used downloaded saves in a disposable profile (provenance in
  `artifacts/lanes/06-dynamic/cont3/SAVE_PROVENANCE.md`).

## Strict-closure evidence

* **Permanent 1280×720 windowed mod.** `tools/mod/kotor_windowed_1280.py` patches a user-owned
  1.0.3.0 `swkotor.exe` at three sites: resolution validation (`FUN_005f0c60`), the tooltip 720
  branch (`0x0062783E`), and the HUD 720 branch (`0x0068C52D`). It verifies the original SHA-256
  `0290889e…`, writes a backup, and supports apply/revert/check. Patched profile SHA-256 is
  `280ba024…`. Required config: `FullScreen=0`, `AllowWindowedMode=1`, 1280×720,
  `Frame Buffer=0`, shadows/grass/AA off. See `docs/recon/16`.
* **Retail save acceptance at 1280×720.** With an elevated foreground session and the
  profile-local Mesa renderer, `000005 - TARIS` loads `MODULES:tar_m02af_s` +
  `CURRENTGAME:tar_m02af` and renders the HUD at 1280×720. In-game F4 writes a valid
  `000000 - QUICKSAVE` (`last_module=tar_m02af`); F5 quickload restores it. The earlier black
  screen was Frame Buffer Effects: with `Frame Buffer=1` the load enters a 120×120 pbuffer path
  where Mesa's `wglMakeCurrent` fails. `Frame Buffer=0` removes it. The client stays
  `[0,0,1280,720]`; the earlier shrink was a harness `SetWindowPos(...,800,600)` call. Evidence:
  `artifacts/lanes/06-dynamic/save-acceptance-taris/cont2/runs/elev-load11-noshrink-20261024` and
  `.../elev-save4-noshot-20261024`. The save-thumbnail resampler still faults under Mesa
  (`FUN_0045dad0` at `0x0045db21`); the acceptance run synthesized `Screen.tga`, a renderer
  residual listed below, not a save-format defect.
* **VFS closing passes.** Measured on 2026-10-24 in the authorized focused windowed session, all
  with `integrity.ok = true`: duplicate-in-archive and KEY first-wins; `.mod` mounted instead of
  `_s`; a mounted global RIM beats `.mod`; save-nested ERF beats `.mod` but loses to `_s`; pass-4
  order save > `.mod` > `LIPS:localization` > `HD0:players` > texture packs; the save→RIM
  converter drops the nested duplicate before `CURRENTGAME` is built (nested `m12ab.sav` 4
  entries including the `VFSDUP` duplicate, converted `currentgame/m12ab.rim` 3 entries); flat
  one-level loose directories; directory enumeration follows OS/NTFS order with no user-space
  sort; pass-1 directory tables are last-added-first (`HD0:STREAMMUSIC` won a four-directory
  contest). Evidence: `docs/recon/08`, `docs/recon/10`,
  `artifacts/lanes/06-dynamic/vfs-precedence/runs2/`.
* **NCS source decompilation.** The clean-room decompiler (`final-v10`) reproduces **8,428/8,666**
  unique scripts byte-identically and with identical instruction streams (97.26%): 228 mismatch,
  5 compile_fail, 5 reachable-only, 0 partial, 0 regressions. All 5 compile_fail are
  header-limited and byte-identical under lane-local action prototypes. The non-exact taxonomy is
  228 compiler-limited (201 struct-global slices, 24 action defaults, 3 body-layout/address-order
  files), 5 header-limited, and 5 reachable-only; **decompiler-side open is 0**. Compiler-limited
  classes are never counted exact. See `docs/recon/09`.
* **MDX and camera.** Every MDX byte reachable from the shipped archives is assigned: 2,832/2,832
  models, 76,703 blocks, 489,549,652 bytes exactly split into payload/sentinel/pad; relocation is
  `runtimePtr = dataHeader + word` (file offset `12 + word`); GPU skinning uses qbone/tbone in
  `program.env[18..68]` with attrs 1/4; aux4 has no consumer. The CPU-skin u16 bone read at
  `0x487b04` is the per-weight path, entered for ordered nonzero weights and skipped for zero/NaN
  at `0x487afe`; the live Taris scene used the GPU skin path. Camera mode `+0x38` is
  construction-time from controller `+0xc4`, which the dialogue-controller constructor zeroes; the
  live probe confirmed controller `+0xc4 = 0` in-process. No dialogue camera is constructed in a
  non-dialogue scene, so the object field itself needs a dialogue/cutscene scene (P1). See
  `docs/recon/12`, `docs/recon/15`,
  `artifacts/lanes/06-dynamic/camera-dynamic/runs/camera-taris-1`.
* **Test suite.** `KOTOR_GAME_DIR=<install> python tools/tests/run_all.py` records **47 tests, 46
  pass, 1 skip, 0 failures/errors**; the skip is the absent real save in the reference install
  (downloaded slots are checked separately). See `artifacts/tests/results/test-results.json`.

## P1/P4 handoff (outside P0)

These are not P0 gate criteria or missing P0 measurements; they are the next phases' work, listed
with the exact boundary so they cannot be mistaken for closed P0 items.

* **Engine renderer (P4/P5).** The C++23/SDL3/Vulkan/OpenGL scaffold clears and draws ImGui only;
  no depth/mesh pipeline, asset rendering, or parity renderer yet.
* **Save thumbnail (P4 renderer compatibility).** `FUN_0045dad0` faults at `0x0045db21` under
  Mesa; a native driver, a Mesa-compatible resampler shim, or a clean-room screenshot replacement
  is required for engine-generated thumbnails. Save data and reload are unaffected.
* **Fresh-module area-RIM order (P1).** Controlled New Game reaches `RIMS:CHARGEN`, but the
  character-generation `PLAY` control did not activate under posted or hardware mouse input,
  arrow keys, Enter, or number hotkeys. The `_a`/`_adx`/`.rim`/`_s` intra-order and `.mod` vs
  area RIM remain unmeasured. The loaded-save path mounts `_s` or `.mod` instead; `_s` > `global`
  and `MAINMENU` > `GLOBAL` are measured.
* **`streamwaves` audio requests (P1 audio).** The directory, one-level scan, and pass-1 index are
  measured; no audio `.wav` request before gameplay was observed.
* **Dialogue-camera object observation (P1).** Static analysis fixes camera `+0x38` at 0 for every
  shipped path, and the live controller `+0xc4 = 0` is confirmed; a dialogue/cutscene scene is
  needed to observe the camera object's field in-process. The mode-nonzero branches are
  unreachable in shipped data.
* **Synthetic multi-group mesh (P1 renderer).** The engine loops are N-group capable (`+0x100`
  counts array, `+0x118` offsets, `+0x11c` group count; arrays relocated by `FUN_004a0c00`), but
  every shipped mesh is group count 1. A synthetic engine load with divergent counts is renderer
  verification, not a shipped-data gap.
* **Bbox expansion provenance (exporter data).** 108/76,703 meshes carry a stored bbox expanded
  beyond the vertex AABB; no code writes it and it matches no transform. The origin is
  exporter/scene-time and not recoverable from the file; the runtime must preserve stored values.
* **Supermodel visual chain (P1).** Paths and supernode binding are verified; a synthetic
  child/super chain with duplicate, ancestor-only, and missing clips is still needed for visual
  output confirmation.
* **Public license and release clearance (owner/P6).** `docs/LEGAL_REVIEW.md` records the
  engineering IP review; license selection and public-release clearance remain owner/release
  decisions.

## Reproduce

```bash
export KOTOR_GAME_DIR="<KOTOR_INSTALL>"
python tools/install/install_validate.py
python tools/install/cache_profile.py build --out artifacts/cache/profile.json
python tools/install/cache_profile.py verify --profile artifacts/cache/profile.json --game-dir "$KOTOR_GAME_DIR"
KOTOR_GAME_DIR="$KOTOR_GAME_DIR" python tools/tests/run_all.py
```

P1 work reuses these measurements. P0 closure rests on the six items above plus the strict-closure
evidence; it does not assert runtime or visual equivalence of the rebuilt engine.
