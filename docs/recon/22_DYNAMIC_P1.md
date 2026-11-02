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
The 2026-11-02 live closure (direct PLAY, module start, first streamwaves voice, F4 thumbnail)
is in §6 and `runs/hw-startgame6-20261102/` + `runs/mesa-nomovies-20261102/` +
`runs/mesa-quicksave7-20261102/`.

## 0. Runs

| run | scenario | outcome |
|---|---|---|
| `a-fresh-newgame` | boot → New Game attempt | menu OK; New Game click missed (1st click only focused) |
| `a2-newgame` | boot → New Game → chargen → PLAY attempts | `RIMS:CHARGEN` loaded (seq 7746); PLAY never activated (early blocker, superseded by the §6 live closure) |
| `b-load-taris` | Load Game → `000005 - TARIS` → gameplay → dialogue clicks → F4/F5 | module loaded (`MODULES:tar_m02af_s`, `CURRENTGAME:tar_m02af`); camera fields observed live; no dialogue started |
| `c-parity`, `c2-parity`, `c3-parity` | Load Game attempts | flaky menu clicks; c3 loaded TARIS again |
| `d-parity-dialogue` | Load Game → `000007 - PARITY` (verbatim rewrite) → gameplay → click grid → F4/F5 | **rewritten save accepted and loaded**; slot files unchanged after load |
| `hw-startgame6` | hardware → quick char → direct `FUN_006eb320` PLAY | `END_M01AA` + `_s` open; ATI `atioglxx.dll` faults on first module draw |
| `mesa-nomovies` | Mesa → quick char → direct PLAY, movies off | Endar Spire runs; live `STREAMWAVES\m01aa\tras02\nm01aatras02000_.wav` |
| `mesa-quicksave7` | Mesa gameplay → direct Quick Save (action `0xDA`) | slot `000000 - QUICKSAVE` with `Screen.tga` 196,626 B; thumbnail shim fired |

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

Fresh New Game path (run a2): `RIMS:CHARGEN` mounted at seq 7746, but posted input never
activated the final PLAY control. **Live closure 2026-11-02:** the visible quick-character
panel is `FUN_006f0390` (0x1360 bytes; step `+0x135c`), whose PLAY path is
`0x6efd60` → `FUN_006eb320([panel+0x68])` → `FUN_006dbdf0(manager)`. `live_hooks.js` captures
the chargen manager in `FUN_006eb420` and invokes the handler on the game thread (no synthetic
click). `hw-startgame6` opens `modules\END_M01AA.rim` and `modules\END_M01AA_s.rim`; the ATI
driver then faults at `atioglxx.dll+0xAE71E7` in the first indexed draw.
`mesa-nomovies-20261102` survives the transition and opens
`STREAMWAVES\m01aa\tras02\nm01aatras02000_.wav` (`ok=true`). Fresh-module `end_m01aa`
intra-order is therefore **observed live**: area RIM `modules\END_M01AA.rim`, static RIM
`modules\END_M01AA_s.rim`, then `currentgame\END_M01AA.rim`; no `.mod` or `_a`/`_adx` files
ship in retail, consistent with the static `FUN_004c44d0` order.

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
Per-file streamwaves audio requests are now **verified live**: `mesa-nomovies-20261102`
opened `.\STREAMWAVES\m01aa\tras02\nm01aatras02000_.wav` with `ok=true` (seq 1479/1480)
after the direct PLAY handler started `END_M01AA`; the scan/index behavior is also verified.

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
3951–3952, but never requested). The live closure below starts the module directly and captures
the first scripted dialogue voice line, so no click targeting is needed for the streamwaves
observation.

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

## 4. Open items (blockers) — superseded by the §6 closure runs below

## 6. Sprint-1 closure runs (consent in force; raw logs under `artifacts/lanes/s1-06-dynamic/runs-sprint1-closure/`)

Build SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`; all runs on
the patched profile copy, one owned instance at a time, install sentinels re-verified after
every run (`ok`, 0 drift, 0 leftover processes). Harness: `tools/harness/sprint1_dynamic_run.py`
+ `sprint1_hooks.js` (Frida), fixture builder `sprint1_fixture_2group.py`; scenarios under
`scenarios/`; interactive driving via the control socket + orca-computer screenshots.

### Task 1 (item a) fresh New Game → `end_m01aa` — **LIVE CLOSED (2026-11-02)**

Runs `j2-newgame`/`j3-newgame`: `RIMS:CHARGEN` mounted (seq 7758) but the first module never
loaded. Approaches tried (all distinct, logged in `chargen-drive*.jsonl`): posted
`clickfast` at lane-06 coords and at GUI-file-derived coords (buttons read from
`mainmenu`/`classsel`/`abchrgen` .gui via `tools/formats/gui.py`: BTN_NEWGAME design
(285,222,210x22), classsel portraits, BTN_ACCEPT (253,410,134x28) in the 640x480 design
space → client (x*1.5+160, y*1.5)); absolute SendInput `hwclick` (fixed to
MOUSEEVENTF_ABSOLUTE); keyboard Enter/Tab/Space; orca-computer screenshots of the chargen
screen. After chargen mounted, **zero** game events were produced by any click (last event
seq 7963) — injected input does not reach the chargen 3D screen in this environment
(DirectInput under llvmpipe), reproducing lane-06's PLAY blocker. Verbatim loader tail:
`[HD0:CHITIN, OVERRIDE:textures, HD0:patch, RIMS:GLOBAL, TEXTUREPACKS:swpc_tex_tpa,
TEXTUREPACKS:swpc_tex_gui, RIMS:MAINMENU, RIMS:CHARGEN]` — no `end_m01aa*` loader.
Loaded-save path remains as measured in §2a. The fresh path's mount order is now **statically
closed** on `FUN_004c44d0` (docs/recon/23 #23–#25): `.mod` probe first, then
`MODULES:<mod>_s`, LIPS, optional `_a`/`_adx`, players. The live repeat is now captured by the direct handler (see §2a and the 2026-11-02 closure runs
below): `hw-startgame6` and `mesa-nomovies-20261102`; no loader ambiguity remains.

### Task 2 (item b) streamwaves voice requests + Task 3 (item c) dialogue — **streamwaves LIVE CLOSED (2026-11-02)**

Run `i-dialogue` (Taris Hideout save loaded, gameplay reached): 89 SendInput grid clicks +
13 posted clicks over the play area (`hunt.jsonl`, `hunt2.jsonl`) — **zero** type-2029 (DLG)
lookups, **zero** `streamwaves\*.wav` file opens, no new camera events. Carth
(`k_hcar_dialog`, indexed at boot from `HD0:CHITIN` addkey seq 3951) was never clicked
successfully; no voice line played. Camera `+0x38`/`+0xc4` live evidence stands from run b
(§2c). Blocker: injected mouse input does not drive the in-game world cursor here (same
DirectInput blocker as task 1). The streamwaves request path itself is now **statically
closed** (docs/recon/23 §D, claim #32): voice streams build `HD0:STREAMWAVES\%s` (or the
localized `%s\%s\%s` form), type 4 opens the `.wav` through `FUN_005dbd30` /
`AIL_open_stream`; music and effects use `STREAMMUSIC`/`STREAMSOUNDS`. A live voice line is now
captured: `mesa-nomovies-20261102` opened `.\STREAMWAVES\m01aa\tras02\nm01aatras02000_.wav`
(`ok=true`, seq 1479/1480) after the direct PLAY handler started `END_M01AA`, so both the path
and the observation are closed.

### 2026-11-02 elevated Mesa runs (input now proven; load/chargen still unstable)

A second pair of runs used an elevated `Start-Process -Verb RunAs` wrapper (UAC disabled on this
host) with the existing Mesa profile. Results:

* `elev-menu-load-20261102`: 45 s boot, `focus`/`clickfast` worked, menu frame captured
  (1280×720, mean 30.75), Load Game click opened the transition, `status` seq 7339, clean
  `application-requested` quit (`0xDEADBEEF`). This **replaces the earlier "no input" claim for
  the elevated path**: input and window focus work.
* `elev-live-taris2-20261102`: menu → Load Game → row click produced a changed 1280×720 frame
  (mean 50.96, `02-selected`), proving the listing selection works; the subsequent module-load
  transition terminated the process (`process-terminated`, crash None) before a gameplay frame
  (`10-loaded` no render window).
* `elev-idle-shim4-20261102`: survived 90 s at the menu and quit cleanly (`application-requested`),
  proving the glGetString shim keeps the menu path alive; `elev-b-load-taris-20261102d` reached
  the menu and accepted clicks but terminated during the load transition.
* A later attempt to add a `wglMakeCurrent` restoration shim caused startup termination during
  the logo movie, so it was **not** kept; the harness now has only the tested glGetString,
  glu, screenshot and resampler shims.

Conclusion: input is no longer the blocker on the elevated path. The later monitor-on hardware
and Mesa runs close the transition: the hardware profile starts `END_M01AA` and then faults in
the ATI driver, while the Mesa llvmpipe profile survives and captures the first voice line. The
fresh-module order and camera/voice data paths are closed (#23–#28, #32). Run artifacts:
`artifacts/lanes/s1-06-dynamic/runs/elev-*` and `runs/hw-*`/`runs/mesa-*`.

A private Win32 desktop was also tried (`SP1_DESKTOP=1`: `CreateDesktopW` + `CreateProcessW`
suspended, Frida attach/hook, then `ResumeThread`). Run `elev-desktop-idle-20261102` created and
resumed the process, emitted `status-0` at seq 7299 (15 s), then the process terminated before
45 s; no render window. A private desktop has no monitor/GL surface, so this is the same
display/context limitation, not an input failure. The monitor-on hardware and Mesa runs below
provide the remaining live captures.

### 2026-11-02 live hardware captures (monitor on)

With the monitor active, the non-Mesa `profile-hw` renders and is stable at 1280×720. The
following live frames were captured (run dirs under `artifacts/lanes/s1-06-dynamic/runs/`):

* `hw-live-taris-20261102`: main menu, Load Game listing (Taris row visible), row selected;
  clicking Load starts the module mount (`CURRENTGAME:tar_m02af`, `MODULES:tar_m02af_s`,
  `m02af` GIT/ARE/PTH) and the process then terminates during the load transition.
* `hw-newgame-20261102`: New Game reaches `CHARACTER GENERATION / CHOOSE YOUR CLASS`; the
  class portraits render and accept the posted click.
* `hw-quickchar-20261102`: selecting a class and `Quick Character` reaches the final chargen
  screen with `(1) PORTRAIT`, `(2) NAME`, `(3) PLAY`; frames `03-quick.bmp` show the screen.
* `hw-name*`/`hw-key*`: attempts to activate `(2) NAME` and `(3) PLAY` with posted mouse
  messages, SendInput hardware clicks, and keyboard/`WM_CHAR` events. The screen remains on
  `(1) PORTRAIT`; the final PLAY control does not accept any of those input paths. This is the
  original chargen PLAY blocker, now isolated to the final control rather than the whole screen.
* Orca computer-use on the live window (1296×759 at 0,0, PID 25796): the accessibility tree
  contains only the top-level window; a synthetic coordinate click at the PLAY row
  `(780,373)` is reported `unverified (synthetic_input)` and the screenshot stays on the
  PORTRAIT/NAME/PLAY screen. The final control ignores accessibility/synthetic input too.
* Direct in-process attempt (`hw-direct-play-20261102`): the harness queued the chargen PLAY
  starter `FUN_006eff80` on the next `wglSwapBuffers`, avoiding the Frida RPC thread. The call
  threw `access violation accessing 0x68`; disassembly shows the caller must load
  `ecx = [name_screen+0x68]` before the call (`0x2f9e2b`/`0x2f9e2e` in `FUN_006f9cd0`),
  and that name-screen object is only created on the name screen. A zero-arg direct call is
  therefore not sufficient; it needs the live name-screen/player object (or the final PLAY
  control's own handler). The live closure below resolves this with the visible quick-char
  panel (`FUN_006f0390`) and its `FUN_006eb320` PLAY handler, not this name-screen path.

The hardware profile also loads the Taris save's module resources and then terminates in the
load transition; the exact crash point is not a data error (all mounts return 1, install
sentinels unchanged, no save writes).

### 2026-11-02 hardware-profile attempt

To separate the llvmpipe input blocker from the game path, a lane-local `profile-hw` copy was
made without the Mesa DLLs (`opengl32.dll`, `libgallium_wgl.dll`, `dxil.dll` removed) and the
harness was run with `SP1_HARDWARE=1` / `SP1_PROFILE=.../profile-hw`, scenario
`b-load-taris.json`. The process booted far enough to load the menu streams (music
`STREAMMUSIC\mus_theme_cult.wav`, texture packs, main-menu RIMs) and installed the new
`resample_shim_0x45dad0` hook, then **terminated without a crash report** at the render/context
transition: `session_detached {"reason": "process-terminated", "crash": "None"}`, `frame` steps
reported `no render window`, and no `swkotor.exe` remained. Run:
`artifacts/lanes/s1-06-dynamic/runs/hw-b-load-taris-20261102/` (summary, events, orchestrator).
Conclusion: the hardware driver path renders chargen but the ATI OpenGL driver faults on the
first module draw (`atioglxx.dll`, see the live closure below); the Mesa llvmpipe profile
survives the transition and provides the live module/dialogue evidence. The fresh-module mount
order and dialogue camera path are closed (#23–#28), and the stream voice path (#32) is now
also observed live.

### 2026-11-02 live closure — direct PLAY, module start, and streamwaves voice

The visible quick-character panel is `FUN_006f0390` (allocated 0x1360 bytes in `FUN_006eb420`),
with its step counter at `+0x135c`; `[panel+0x68]` is the chargen manager. Its PLAY path is
`0x6efd60` → `FUN_006eb320([panel+0x68])` → `FUN_006dbdf0(manager)`, the same `FUN_006eb320`
used by the name dialog END_BTN and the step-6 handler. `live_hooks.js` captures the manager in
`FUN_006eb420` and calls `FUN_006eb320` on the game thread, so no synthetic click is required.

* `hw-startgame6-20261102` (hardware): `startgame_called` then `modules\END_M01AA.rim` /
  `modules\END_M01AA_s.rim`; the ATI OpenGL driver faults at `atioglxx.dll+0xAE71E7` in the
  first indexed draw (`glDrawElements` via `FUN_00425ab0`/`FUN_0046f900`). The transition itself
  is proven; the crash is driver-side.
* `mesa-nomovies-20261102` (Mesa llvmpipe, `Disable Movies=1`): survives the transition and runs
  the Endar Spire. Verbatim: `startgame_called` seq 358, `modules\END_M01AA.rim` seq 361,
  `modules\END_M01AA_s.rim` seq 377,
  `STREAMWAVES\m01aa\tras02\nm01aatras02000_.wav` seq 1479/1480 `ok=true`.
  `summary.json`: `install_ok_after=true`, `new_save_files=[]`, `changed_save_files=[]`,
  `game_processes_after=[]`.

### Task 4 (item d) from-scratch parity slot — **VERIFIED (load acceptance); F4 thumbnail LIVE-VERIFIED (2026-11-02)**

Run `h-paritysrc` loaded `000008 - PARITY-SRC` (s1-05 `kotor_save_parity.py rewrite-parity
--mode parity` from-scratch output, staged as a fresh slot) via the normal Load Game UI:

```
seq 7314 file CreateFileW .\saves\000008 - PARITY-SRC\SAVEGAME.sav ok=true
seq 7888 loader_rim MODULES:tar_m02af_s
seq 7896 loader_erf CURRENTGAME:tar_m02af
seq 7916 resman module 2014 ret 1 tableName "CURRENTGAME:tar_m02af"
run summary: install_ok_after true, changed_save_files [], detach application-requested (clean quit 0xDEADBEEF)
```

— **verified**: the original engine accepts and boots the from-scratch rewritten save
(verbatim mode verified in run d, §2d).

F4 quicksave thumbnail (2026-11-02, run `mesa-quicksave7`): the direct Quick Save action
`0xDA` was invoked after 90 s of player control; the engine called the save writer
(`quicksave_called`, `save_writer` self=`0xfe013c8`, slot 0), then `FUN_00401080` →
`0x420f20` with the path `.\saves\000000 - QUICKSAVE\Screen`. The live shim fired:
`thumb_replace_0x420f20` and `screen_tga_synth` with 196,626 bytes written. The slot contains
`GLOBALVARS.res`, `PARTYTABLE.res`, `SAVEGAME.sav`, `Screen.tga`, `savenfo.res`; `Screen.tga`
is 256×256, 24 bpp, 196,626 B (same size as the retail TARIS thumbnail). `summary.json`:
`install_ok_after=true`, `new_save_files` includes `Screen.tga`, `changed_save_files=[]`.
The earlier k3/k4 runs invoked Quick Save during the intro cutscene, so the save writer was
never reached; the live path now waits for player control.

### Tasks 5–6 — see `docs/recon/25_SYNTHETIC_RUNTIME.md` (2-group relocation VERIFIED live;
supermodel clip chain VERIFIED live with 579 super-recursions).
