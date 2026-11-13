# Launcher (Sprint 15, lane s15-04-installer)

Status: **implemented**; cache build with progress and cancel is **not done** (see below).

`kotor_launcher` is a small ImGui window (SDL3 + OpenGL 3.3) beside the engine. It follows ADR 0003: it
points at a user-owned install, validates the data set, keeps the options and starts the engine. It never
writes to the install, never runs or reads the original executable, and ships no game artwork, text or font.

## What it does

- **Detects installs** (`engine/src/launcher/InstallDetect.*`). Steam (registry `SteamPath`,
  `steamapps/libraryfolders.vdf`, `appmanifest_<appid>.acf` `installdir`), GOG and original-installer registry
  keys (read-only, 32-bit view), `KOTOR_INSTALL`, `SteamLibrary`/`Steam`/`GOG Games` folders on fixed drives, plus
  folders the player browsed to (remembered in `launcher.ini`). Detection takes its places as input, so tests
  run on fixtures. Games are rows in `gameCatalog()`: KotOR 1 is launchable, KotOR 2 is detected and named but
  not launchable until Sprint 18. A new game is one more row.
- **Validates the data set, not the executable.** Required files and folders per game, `chitin.key` structure
  (every archive it names must exist; a size that differs from the declared one is a warning), `dialog.tlk`
  header and language, `chitin.key` SHA-256 against known data sets, and `Override` content counted as a
  modded-install warning. Unknown hashes and mods are warnings, never rejections (ADR 0003 item 5).
  `Install`/`GameProfile::detect` now also identify an install by its `chitin.key` hash when no executable is
  present, so an executable-free install actually launches.
- **Options screen** (`LauncherConfig.*`). Every engine setting in `ke::UserSettings` (now in
  `engine/src/game/UserSettings.*`, written atomically) as a toggle, slider or choice, plus launch options
  (renderer, language, saves folder) and the remaster section: **Classic** (default, the scaled original font)
  or **Native font**, a `.ttf`/`.otf`/`.ttc` the player picks from the system font folders or browses to. No font
  is bundled (legal decision pending, s15-03). The window shows a live preview of the chosen font.
- **Launches the engine** with `--install`, `--settings-file`, `--saves-dir`, `--backend=`, `--language`,
  `--module`/`--gui-screen` (start on the main menu) and `--ui-font`. The launcher saves, starts the engine and
  exits, so the settings file is never written by two processes.

Command line: `--list`, `--check <folder>`, `--print-command`, `--config-file`, `--settings-file`,
`--saves-dir`, `--engine`, `--install`. Tests pass the first three so nothing reaches the player's profile.

## Not done

- **Native font rendering in the engine.** The engine accepts `--ui-font` and logs that it still draws the classic
  font; the `GuiPainter` hook is a separate change (it touches the interface renderer other lanes edit).
- **External content-addressed cache with progress and cancel.** `tools/install/cache_profile.py` still builds
  the profile; the launcher does not run it yet.
- Hash-level version/language identification beyond the single recorded KotOR 1 data set, and GOG/other
  language data sets (no samples to record).
- The GOG product ids and the original-installer registry keys in the catalog are from memory and were not
  checked on a machine that has those installs.
