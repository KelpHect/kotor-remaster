# Launcher (Sprint 15, lane s15-04-installer)

Status: **implemented**; cache build with progress and cancel is **not done** (see below).

## Rust launcher (`launcher/`, GPUI) - replaces the ImGui window once accepted

`launcher/` is a standalone Cargo crate (not part of the CMake build) that ports the ImGui launcher below to
Rust with a GPUI window: custom title bar, game rail with cards, banner, details and info panel (engine
version from `engine/CMakeLists.txt`, resolution and graphics API from the settings), Play with its ready
state, Verify Files (runs the validation and shows the report), Game Settings, install location with Change,
and a Settings view with every engine option plus the Launch and Remaster sections. Behaviour is the same
code path as the C++ files: detection (`src/install.rs`), validation without opening the executable,
`launcher.ini` and `settings.ini` in the exact byte format of `UserSettings` (CRLF, `%g` floats, atomic
rename; `tests/fixtures/*_cpp.ini` are written by the C++ class through `tests/fixtures/gen_settings.cpp`),
and the same engine command line (`src/config.rs`).

- Build: `cargo build --release` in `launcher/` (`CARGO_TARGET_DIR` outside the repo). Static CRT, GUI
  subsystem, fat LTO, `panic=abort`, stripped. `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings`
  and `cargo test` must pass. `launcher/.cargo/config.toml` turns off the machine-wide `rustc-wrapper`
  (kache) for this crate because cc-rs then routes C compiles through it and kache rejects `cl.exe`.
- Art: nothing is shipped. When the player's own Steam client has cached a game's images
  (`Steam/appcache/librarycache/<appid>/library_hero.jpg`, `header.jpg`), they are shown read-only;
  otherwise an abstract backdrop is drawn in code. Game names are plain text.
- Font: Barlow (SIL OFL 1.1, `launcher/assets/fonts/OFL.txt`), embedded. Icons are the launcher's own SVGs.
- Product name: the docs name none (ADR 0003 open question 4); `PRODUCT_NAME` in `launcher/src/theme.rs` is
  a neutral placeholder.
- News: `launcher/src/news.rs` has a `NewsSource` trait with a placeholder source ("No news yet"); a web
  API source replaces it later. No network access.
- Engine: `--engine` wins; else the path the player located (Locate engine on Home, or Settings > Launch >
  Engine), kept as `engine` in `launcher.ini`; else `kotor_engine.exe` beside the launcher, the current folder
  or the checkout it was built from; else `engine/build/<preset>/kotor_engine.exe` in any of their ancestors
  (release presets first, then the newest); else, for a git worktree, the main checkout's build tree. The
  engine is started without a console window (`CREATE_NO_WINDOW`, null standard handles).
- Layout follows the mockup at its 1536x850 window (`launcher/src/ui.rs` constants). The non-affiliation
  text lives in the About dialog (footer links and emblem) and in Settings > About.
- Command line: the C++ flags (`--list`, `--check`, `--print-command`, `--config-file`, `--settings-file`,
  `--saves-dir`, `--engine`, `--install`) plus `--view home|settings`, `--no-system-scan`, `--steam-root`,
  `--probe-file`, `--probe-exit`.

To remove once the Rust launcher is accepted: `engine/src/launcher/` (`main.cpp`, `InstallDetect.*`,
`LauncherConfig.*`), the `kotor_launcher_core`, `kotor_launcher` and `kotor_launcher_tests` targets and the
`kotor_launcher` / `kotor_launcher_cli` CTest entries in `engine/CMakeLists.txt` (and `kotor_imgui` if nothing
else links it), `engine/tests/test_launcher.cpp` (its checks are ported to `launcher/src/*` tests). Keep
`engine/src/game/UserSettings.*`: the engine itself reads the settings file.

## ImGui launcher (`engine/src/launcher/`)

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
