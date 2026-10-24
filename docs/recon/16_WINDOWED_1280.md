# 16 — True windowed 1280×720 on a 2560×1440 desktop

Status: **implemented and verified** (main menu, Taris load, save write/reload) at a true
1280×720 windowed client on a 2560×1440 desktop.

## 1. Problem

KotOR 1.0.3.0 has no 1280×720 option and does not expose a true windowed 16:9 mode.
`FullScreen=0` alone still produced an 800×600 window on a 2560×1440 desktop.

## 2. Engine path (image base 0x00400000)

- `FUN_005f0ce0` @ `0x005f0ce0` reads `[Graphics Options]` `Width`, `Height`,
  `AllowHighMonitorFrequency`, and `RefreshRate` from `.\swkotor.ini`, then validates the
  size.
- `FUN_005f0c60` @ `0x005f0c60` accepts only 800×600, 1024×768, 1280×960, 1280×1024, and
  1600×1200. Any other pair is replaced by 800×600. This is the reason a requested
  1280×720 became an 800×600 window.
- `AllowWindowedMode` is read into `DAT_007a3a30`. When it is zero, windowed mode is forced
  to fullscreen. `FullScreen=0` selects renderer mode 1 (windowed); `FullScreen=1` selects
  mode 2 (fullscreen) through `FUN_005f5ab0` @ `0x005f5ab0`.
- `FUN_005f1830` @ `0x005f1830` resolves the exact `EnumDisplaySettingsA` mode by
  width/height/bpp/refresh, writes `screenW`/`screenH` (`0x0078d1d4`/`0x0078d1d8`), and
  calls the window/renderer setup `FUN_00403800`.
- Renderer `FUN_0044dab0` @ `0x0044dab0` creates the windowed window with style `0x2CF0000`,
  a client area equal to the requested width/height, and `ShowWindow(1)`. Fullscreen uses
  style `0x82000000` and `ChangeDisplaySettingsA`.
- The desktop exposes 1280×720×32 at 50/59/60/120/144/165/170/180 Hz, so the engine-side
  DEVMODE lookup succeeds once validation is open.

## 3. 1280×720-specific engine bugs

Two GUI constructors select resolution-specific resources and have no 1280×720 case:

- `FUN_006277c0` @ `0x006277c0` (tooltip): width 1280 only handles height 960/1024. For 720
  it jumps over initialization and passes an uninitialized name to the resource manager.
  The observed result was a null-`this` access violation in `FUN_00411630` at `0x00411645`.
- `FUN_0068c100` @ `0x0068c100` (HUD): the same missing 1280×720 branch skips the 1280 HUD
  resource selection.

## 4. Fix

`tools/mod/kotor_windowed_1280.py` patches the user's own 1.0.3.0 executable:

- extends `.text` VirtualSize to `0x33C000` (the existing raw padding; no section overlap);
- installs three trampolines in `.text` padding at `0x73C1D0`, `0x73C210`, `0x73C250`;
- patches:
  - `0x005F0C60` — accept 1280×720 while retaining every shipped mode;
  - `0x0062783E` — route 1280×720 to `tooltip12X9`, retaining 960/1024;
  - `0x0068C52D` — route 1280×720 to the 1280 HUD resource, retaining 960/1024;
- verifies the original SHA-256, writes a backup (`swkotor.exe.orig-1.0.3.0`), and supports
  `--apply`, `--revert`, and `--check`.

Required config:

```ini
[Graphics Options]
FullScreen=0
AllowWindowedMode=1
Width=1280
Height=720
RefreshRate=60
Frame Buffer=0
Shadows=0
Soft Shadows=0
Grass=0
Anti Aliasing=0
```

`Frame Buffer=0` is required on modern systems: with it enabled, KotOR's offscreen/pbuffer path
fails under current drivers and the save load ends in a black screen. The remaining effect keys
avoid the same renderer path.

## 5. Verification

- Patched exe SHA-256 `280ba024d7410933ef1dad5ed829e9bef428c07e7ae47d8a6ad7a3a9420d49e3`
  (original `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`).
- Early-spawn probe: validator returned 1 natively for 1280×720 with the mod (hook disabled);
  `screenW/H` stayed 1280×720; window style `0x2CF0000`; window 1296×759 with client
  1280×720 at client origin (8,31).
- Frida-free direct launch on the 2560×1440 desktop reached the main menu; capture
  `artifacts/lanes/06-dynamic/windowed-mod/21-direct-menu-1280.png`.
- Install sentinels and all six save slots unchanged.
- The menu is composed as 4:3 content (960×720) centered inside 16:9; the surrounding frame
  fills the window. This is the engine's composition, not a size error.
- Save acceptance (2026-10-24): `000005 - TARIS` loads (`MODULES:tar_m02af_s`,
  `CURRENTGAME:tar_m02af`) with the full HUD at 1280×720; in-game F4 quicksave creates
  `000000 - QUICKSAVE` with `last_module=tar_m02af`, and F5 quickload restores the HUD.
  The main client stays `[0,0,1280,720]` through load and reload. Evidence:
  `.../save-acceptance-taris/cont2/runs/elev-load11-noshrink-20261024` and
  `.../elev-save4-noshot-20261024`.

Evidence index: `artifacts/lanes/06-dynamic/windowed-mod/summary.md`.

## 6. Residuals

- **Save thumbnail capture:** the engine screenshot resampler `FUN_0045dad0` faults at
  `0x0045db21` under Mesa while generating `Screen.tga`. The acceptance run skipped the
  screenshot body in the lane hook and synthesized a 256×256 `Screen.tga` from the HUD frame;
  the save data and reload are unaffected. A native driver or a clean-room screenshot shim is
  still needed for engine-generated thumbnails.
- **Elevation/foreground:** the acceptance run required an elevated session and immediate
  in-process clicks; delayed clicks only hovered. This is test-harness behavior, not a game
  requirement.
- In-game visual check of `tooltip12X9` and the 1280 HUD resource at 16:9; the 4:3 variants
  are the closest shipped assets.
- `FUN_0040b360` also branches on `screenW` (font suffix selection) but only by size
  thresholds; it needs no 1280×720 case.
- VFS precedence residuals: duplicate drop site, LIPS-vs-players, KEY duplicates, directory
  enumeration order, and pass-1 last-added-first are now measured; only fresh-module area-RIM
  order (character-generation PLAY input blocker) and streamwaves audio requests remain tracked in
  `docs/recon/08_VFS_DYNAMIC.md` and `docs/recon/10_SAVE_VFS.md`.

## 7. Sources consulted

- Reddit r/kotor: “Disable frame buffer effects and the load should succeed.”
  <https://www.reddit.com/r/kotor/comments/yufsx9/i_just_got_the_game_of_steam_and_it_doesnt_want/>
- WineHQ bug 59555: KotOR loading failure only with framebuffer effects enabled.
  <https://bugs.winehq.org/show_bug.cgi?id=59555>
- Deadly Stream: “Frame Buffer effects is the problem.”
  <https://deadlystream.com/topic/3199-black-screen-issue/>
- WSGF KotOR article: pre-rendered movies are hardcoded to 800×600; UniWS alters this.
  <https://www.wsgf.org/article/star-wars-knights-old-republic>
- KotOR keyboard controls (F4 quicksave).
  <https://starwars.neoseeker.com/wiki/KotOR_Keyboard_Controls>

Clean-room note: the tool patches the user's own copy and ships no original executable,
assets, saves, or extracted game data.
