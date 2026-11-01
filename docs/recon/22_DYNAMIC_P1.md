# 22 — Dynamic P1 measurements (lane s1-06-dynamic, Sprint 1 gate G1 item 6)

Status: **run under integrator display consent ("Focused main-screen run OK",
`artifacts/lanes/s1-06-dynamic/CONSENT`).** Four owned game instances were run
sequentially (one at a time) on the windowed-1280x720 patched COPY
(`artifacts/lanes/s1-06-dynamic/profile/swkotor.exe`, SHA-256
`280ba024d7410933ef1dad5ed829e9bef428c07e7ae47d8a6ad7a3a9420d49e3`); the install was never
written (sentinel re-verified before and after every run, 0 drift).
Target build SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
(image base `0x400000`). Instrumentation: `tools/harness/sprint1_hooks.js` (Frida, the
lane-06 hook style) driven by `tools/harness/sprint1_dynamic_run.py`; camera-site VAs
re-verified statically via `tools.mcp_hydralysis.disasm` (output header carries the build
SHA-256 above). Raw logs: `artifacts/lanes/s1-06-dynamic/runs/<label>/events.jsonl`.

## 0. Runs

| run | scenario | outcome |
|---|---|---|
| `a-fresh-newgame` | boot → New Game attempt | menu OK; New Game click missed (1st click only focused) |
| `a2-newgame` | boot → New Game → chargen → PLAY attempts | `RIMS:CHARGEN` loaded (seq 7746); PLAY never activated (blocker reproduced, §2a) |
| `b-load-taris` | Load Game → `000005 - TARIS` → gameplay → dialogue clicks → F4/F5 | module loaded (`MODULES:tar_m02af_s`, `CURRENTGAME:tar_m02af`); camera fields observed live; no dialogue started |
| `c-parity`, `c2-parity`, `c3-parity` | Load Game attempts | flaky menu clicks; c3 loaded TARIS again |
| `d-parity-dialogue` | Load Game → `000007 - PARITY` (verbatim rewrite) → gameplay → click grid → F4/F5 | **rewritten save accepted and loaded**; slot files unchanged after load |

Every run ended with `install_ok_after: true`, `game_processes_after: []`, no original
save touched (fresh slots only; `saves-before/after` trees in each `runs/<label>/`).

## 1. Boot resolver order (re-verified live, run a2; matches docs/recon/08)

Verbatim loader events (image base 0x400000, hooks `loader_key 0x40fb80`,
`loader_erf 0x40f3c0`, `loader_rim 0x40f990`):

```
seq 26    loader_key HD0:CHITIN
seq 3980  loader_erf OVERRIDE:textures   (absent in retail; empty table)
seq 3992  loader_erf HD0:patch
seq 6449  loader_rim RIMS:GLOBAL
seq 6963  loader_erf TEXTUREPACKS:swpc_tex_tpa
seq 6975  loader_erf TEXTUREPACKS:swpc_tex_gui
seq 7181  loader_rim RIMS:MAINMENU
seq 7746  loader_rim RIMS:CHARGEN        (after New Game click)
```

— **verified**. Pass-level lookups for `mainmenu` (run a, seq 7311–7357) show the
five-pass sweep per lookup: filter 0 (directories) miss → filter 1 (tag-1 ERFs) →
filter 0 (RIMs) hit `RIMS:MAINMENU` for `mainmenu`/2002; `mainmenu`/2047 hit
`HD0:patch` at filter 1; `mainmenu`/3008 hit `HD0:CHITIN` at filter 0 of pass 5.

## 2. Measurements

### (a) Area-RIM intra-order — loaded-save path measured; fresh path blocked

Loaded-save path (run b, `000005 - TARIS` = module `tar_m02af`), verbatim loader events:

```
seq 7380  loader_erf HD0:players
seq 7394  loader_erf LIPS:tar_m02af_loc
seq 7402  loader_erf LIPS:localization
seq 7884  loader_rim MODULES:tar_m02af_s
seq 7892  loader_erf CURRENTGAME:tar_m02af
```

Pass/resman evidence (run b): `tar_m02af`/2011 (`.mod` probe) missed all five passes
(seq 7878–7883, `resman ret 0`); `module`/2014 hit `CURRENTGAME:tar_m02af` (seq
7911–7912); `m02af`/3003 (PTH) hit `MODULES:tar_m02af_s` (seq 7962–7963); `m02af`/3000
and /3001 hit `HD0:CHITIN` (seq 7940–7947, 8813–8820); `tar_m02af`/2057 hit
`GAMEINPROGRESS:` (seq 7366–7367) then `CURRENTGAME:` (7392–7393).
— **verified**: on the loaded-save path the area static RIM `_s` is mounted as
`MODULES:tar_m02af_s` and the save ERF as `CURRENTGAME:tar_m02af`; no `modules/tar_m02af.rim`
loader fired and no `.mod` exists. The engine's `MODULES:` index enumerates every module
RIM pair at load (addkey `MODULES: tat_m17ac 3002`, …, seq 7802+).

Fresh New Game path (run a2): `RIMS:CHARGEN` mounted at seq 7746, then **no further
loader events** — the chargen `PLAY` control did not activate. Inputs tried, in order:
`clickfast(742,352)` x2 (New Game), `clickfast(768,250)` (quick create), `key(Enter)` x3,
`key('3')`, `clickfast(1000,666)`, `clickfast(742,640)` — posted WM_* messages with
in-process focus (plus a prior run with SendInput/hardware click). This reproduces the
lane-06 blocker exactly (docs/recon/08 "Remaining inferred / open"). — fresh-module
`end_m01aa` intra-order (`_a`/`_adx`/`.rim`/`_s` open order, `.mod` vs area RIM on a fresh
load) remains **open**, blocker: chargen PLAY input + no `_a`/`_adx`/`.mod` files ship in
retail (read-only `ls` of `modules/` and `rims/`), so only the loaded-save variant is
observable in this install.

### (b) streamwaves — directory scan and pass-1 index measured; no streamwaves audio request served

Boot directory scans (run a, verbatim):

```
seq 27    dir_close .\override\*.*        count 4    wavs 0
seq 4010  dir_close .\MOVIES\*.*          count 126  wavs 0
seq 4991  dir_close .\STREAMWAVES\*.*     count 1144 wavs 978
seq 6210  dir_close .\STREAMMUSIC\*.*     count 242  wavs 238
```

— **verified**: `streamwaves/` is scanned once at boot, flat, OS order, and indexed into
the pass-1 `HD0:STREAMWAVES` table (raw `FindNextFile` counts: 1,144 entries of which 978
match `\.wav$`; 1,216 `dirnext_wav` events logged; the install's `streamwaves/` holds 570
entries per read-only `ls`). During menu → module →
gameplay (runs a/b/d) **zero** `streamwaves\*.wav` file opens occurred; all `.wav` file
opens were `streammusic` (`mus_theme_cult.wav` menu, seq 7370–7371; `mus_theme_bast.wav`
+ `al_en_uprclasapt.wav` in-game, seq 8889–8896). Type-4 resman lookups resolved from
`RIMS:GLOBAL` (`gui_click`/`gui_scroll`… seq 7013–7045; footsteps `fs_metal_soft1/2/3`
seq 8933–8938) and `HD0:CHITIN` (`dr_lts02_open` seq 8942–8943); `computer_lp_04` missed
(ret 0, seq 7989–7990 — confirmed absent from retail `streamwaves/`, read-only `ls`).
Per-file streamwaves audio *requests* therefore remain **open** (no voice line was
triggered in these runs); the scan/index behavior is **verified**.

### (c) Dialogue camera +0x38 / controller +0xc4 — observed live (camera path); dialogue start open

Static VA check (hydralysis `disasm`, header `sha256 0290889e…74e6`): `0x6bb6f8:
mov dword ptr [esi+0x38], ecx`; `0x630244: mov dword ptr [esi+0xc4], ebx`;
`0x62bf4e/0x6320c9/0x632127: mov ecx, dword ptr [esi+0xc4]` → `mov ecx, eax` →
`call 0x2412f0` (factory `FUN_006412f0`).

Live events (run b, verbatim):

```
seq 7039  controller_mode_write ctrl=0x4d64f20 value=0        (0x630244)
seq 7040  controller_ctor leave ctrl=0x4d64f20 c4=0 f38=0 f34=0 (0x62fed0)
seq 8939  camera_mode_read site=0x6320c9 controller=0x4d64f20 c4=0 camera=0x4d1fe68
seq 8940  camera_mode_write cam=0x10bc6080 value=0           (0x6bb6f8)
seq 8941  camera_ctor leave cam=0x10bc6080 mode38=0 closeup34=0 vtable=0x756ae8 (0x6bb670)
```

— **verified**: in-process, the dialogue-controller ctor zeroes controller `+0xc4`, the
camera-creation read at `0x6320c9` sees `+0xc4 == 0`, and the dialogue-camera object
(vtable `0x756ae8`, matching docs/recon/12) gets `+0x38 = 0` at construction. This fired
during module load/gameplay camera setup, not inside a conversation: no DLG (type 2029)
resman lookup occurred in any run (13 clickfast grid points over the play area in run d
plus 5 in run b; Carth `k_hcar_dialog` is indexed at boot from `HD0:CHITIN`, addkey seq
3951–3952, but never requested). Starting an actual dialogue remains **open**, blocker:
blind posted clicks did not hit the NPC (no `k_hcar_dialog`/2029 lookup); needs either
visual targeting of the game window or a scripted trigger.

### (d) Rewritten-save retail acceptance (s1-05 writer, docs/recon/21 §4)

The s1-05 parity writer output was staged as fresh profile slots:
`000007 - PARITY` (`rewrite-parity --mode verbatim`: 5/5 files + nested byte-identical,
`saveprep/rewrite-verbatim.json`) and `000008 - PARITY-SRC` (`--mode parity` from-scratch:
2/5 byte-identical, `saveprep/rewrite-parity.json`), copied from the lane-06 retail
quicksave `000000 - QUICKSAVE`.

Run d loaded **`000007 - PARITY`** through the normal Load Game UI — verbatim evidence:

```
seq 7304  file CreateFileW .\saves\000007 - PARITY\SAVEGAME.sav ok=true (GENERIC_READ)
seq 7878  loader_rim MODULES:tar_m02af_s
seq 7886  loader_erf CURRENTGAME:tar_m02af
run summary: "changed_save_files": [], "new_save_files": [], install_ok_after: true
```

— **verified**: the original engine (patched windowed copy) parsed and booted the
verbatim-rewritten save into `tar_m02af` gameplay (module mounted, in-game sounds at
seq 7983–7990) and did not modify the source slot. The from-scratch `--mode parity`
variant (`000008 - PARITY-SRC`) and the F5 reload leg remain **open**: F4 quicksave did
not materialize a `000000 - QUICKSAVE` slot in this harness (only the `gameinprogress`
write at seq 8921–8922; the Mesa thumbnail resampler is ret-patched at `0x420f20`, event
seq 14, and no `Screen.tga` was synthesized this run, so the slot write did not complete).
Lane-06 already proved load→HUD→F4→F5 at 1280×720 for the engine-written quicksave
(docs/recon/16 §5).

## 3. Tooling added (this lane's new files)

- `tools/harness/sprint1_probes.py` — preflight (sentinel/consent/single-instance),
  profile builder (patched copy + ini + junctions, fresh save slots), manifest builder,
  plan writer, `vfs_measure` compare wrapper, consent-gated `run`.
- `tools/harness/sprint1_hooks.js` — Frida instrumentation (loaders/AddKey/pass/resman,
  file + directory enumeration, camera +0x38/+0xc4 watchers, disclosed shims:
  screenshot ret `0x420f20`, `gluBuild2DMipmaps`→`GL_GENERATE_MIPMAP` under Mesa llvmpipe).
- `tools/harness/sprint1_dynamic_run.py` — one-owned-instance driver (Frida spawn suspended
  → hooks → resume; loopback control socket; posted + SendInput input; game-window-only
  PrintWindow captures; post-run sentinel re-check).
- Artifacts: `artifacts/lanes/s1-06-dynamic/` — `preflight.json`, `runtime-status.json`,
  `profile/`, `manifests/`, `probe_plan.json`, `scenarios/`, `saveprep/`,
  `runs/{a-fresh-newgame,a2-newgame,b-load-taris,c-parity,c2-parity,c3-parity,d-parity-dialogue}/`
  (each with `events.jsonl`, `orchestrator.json`, `saves-before.json`, `summary.json`,
  `frames/`), `logs/`, `comparison/`.

## 4. Open items (blockers)

1. (a) fresh-module `end_m01aa` area-RIM intra-order — **open**: chargen PLAY input
   blocker reproduced verbatim (§2a); no `_a`/`_adx`/`.mod` ship in retail.
2. (b) per-file streamwaves audio requests — **open**: no voice line triggered; scan +
   pass-1 index verified (§2b).
3. (c) dialogue-started camera observation — **open**: fields observed live on the
   module-load camera path (§2c); no DLG lookup occurred (blind clicks didn't hit Carth).
4. (d) `--mode parity` (from-scratch) slot acceptance + F5 reload leg — **open**:
   verbatim slot accepted (§2d); F4 slot write didn't complete without a synthesized
   `Screen.tga`.

Stop rule: no repeated identical failures — the PLAY-input blocker was hit twice with
identical result and is now documented exactly (§2a) per the stop rule.
