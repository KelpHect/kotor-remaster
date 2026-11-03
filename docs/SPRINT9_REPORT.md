# Sprint 9 report - original save compatibility and game profiles

Status: **complete - gate S9 met (2026-11-03)**. Sprint 9 adds the compatibility
layer the rest of the engine needs: a first-class KotOR 1 / KotOR 2 game
profile, and native read/write support for the original `SAVEGAME.sav` folder
(outer ERF, nested module ERFs, `PARTYTABLE.res`, `GLOBALVARS.res`,
`savenfo.res`, `Screen.tga`). The engine keeps using its own C++ data layer; the
Python save lane remains the independent validator.

## Environment

- Install: `E:/SteamLibrary/steamapps/common/swkotor`
- `swkotor.exe` SHA-256: `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
- KotOR 2 install: `E:/SteamLibrary/steamapps/common/Knights of the Old Republic II`
- `swkotor2.exe` SHA-256: `6a522e71631dcee93467bd2010f3b23d9145326e1e2e89305f13ab104dbbffef`
- Compiler: MSVC 19.51.36231 (C++23), CMake preset `msvc-release`

## 1. Game profile

`engine/src/kotor/Profile.*` detects the shipped executable and build:

```text
game profile: kind=kotor1 name=Knights of the Old Republic executable=swkotor.exe
sha256=0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6
recognized=1 world=1 original_saves=1

game profile: kind=kotor2 name=Knights of the Old Republic II executable=swkotor2.exe
sha256=6a522e71631dcee93467bd2010f3b23d9145326e1e2e89305f13ab104dbbffef
recognized=1 world=0 original_saves=0
```

`Install` now uses the detected profile for its executable/build check instead
of a single hard-coded `swkotor.exe` hash. KotOR 2 is recognized but explicitly
not claimed as a world-load target in this sprint.

Evidence: `docs/evidence/s9/profile-k1.log`, `profile-k2.log`.

## 2. Original save read

The native reader opens a real retail save and maps module, entry position,
PC sheet, inventory (carried + equipped), party, journal, GVT globals (numbers,
booleans, strings and location vectors) and saved GIT object state.

Real saves loaded:

```text
000000 - QUICKSAVE  tar_m02af  gold=51   inventory=15  journal=4  globals=1180 + 5 locations
000006 - TARISLC    tar_m03aa  gold=731  inventory=16  journal=9  globals=1180 + 5 locations
```

Taris Lower City also proves party loading (`companions=1`) from `PT_MEMBERS`,
and its outer save contains seven nested module ERFs; the reader selects the
`LASTMODULE` entry (`tar_m03aa`).

Evidence: `original-save-load.log`, `original-save-party.log`.

## 3. Original save write

The native writer emits a complete slot from live state:

- `SAVEGAME.sav` (MOD V1.0) with a nested module ERF (`Module` IFO, area GIT,
  area ARE);
- `PARTYTABLE.res`, `GLOBALVARS.res`, `savenfo.res`, `Screen.tga`.

GIT instances are flattened from their shipped templates before the live
position/HP/dead/open/locked/loot state is written. Equipped items go to the PC
`Equip_ItemList`; carried items go to `INVENTORY`. Location globals are
preserved. A fresh `end_m01aa` save is generated without any source save.

Independent Python validation of the generated slots:

```text
docs/evidence/s9/validate-roundtrip.log   validation: PASS (no issues)
docs/evidence/s9/validate-create.log      validation: PASS (no issues)
docs/evidence/s9/validate-party.log       validation: PASS (0 errors, 3 inherited
                                          Mod_Tag warnings from preserved visited modules)
```

The Taris Lower City source itself has four `Mod_Tag` warnings; the writer preserves
those visited-module ERFs verbatim, so three of the four remain. The current module
(`tar_m03aa`) and all sidecars validate cleanly; no new error is introduced.

The round-trip and fresh-create slots are committed under
`docs/evidence/s9/saves/`; the multi-module party slot is represented by its
validation and parse logs because it embeds six preserved retail module ERFs.

## 4. Round trip

```text
original save roundtrip: module=tar_m02af gold=51 inventory=15 journal=4
globals=1185 companions=0 reload=1 -> PASS

original save roundtrip: module=tar_m03aa gold=731 inventory=16 journal=9
globals=1185 companions=1 reload=1 -> PASS

original save create: module=end_m01aa inventory=1 journal=1 reload=1 -> PASS
```

The source save folder hashes are identical before and after the gate:
`docs/evidence/s9/source-before.json` and `source-after.json`,
`SOURCE_UNCHANGED=1`.

## 5. Regression

- **CTest: 33/33 passed** (42.52 s), including the 117-module sweep and VM corpus.
- **117/117 modules**, 0 failures, 0 warnings, 11,887 entities, 51 encounter volumes.
- **VM corpus: 8,666/8,666** default and functional, 0 mismatches, 303,616 ACTION calls.
- **GL/Vulkan scene parity**: mean absolute channel difference **0.0300**,
  verdict PASS (`attempts=1`, `attempt_means=0.0300`). The checker now
  re-captures once when a transient window occlusion produces a large diff and
  reports every attempt mean instead of silently accepting the first frame.
- **Python tools**: 143 tests, 7 skips, 0 failures.
- **Install sentinel**: 15,380 files, 0 session changes.

Evidence: `docs/evidence/s9/ctest.log`, `all-modules.log`, `vm-corpus.log`,
`parity.log`, `tools-tests.log`, `install-sentinel.log`.

## 6. Not in Sprint 9

These are deliberate Sprint 10+ work, not gaps hidden by this gate:

- KotOR 2 world/module loading (the profile is detected; K2 data is gated).
- Faction reputation simulation: a loaded slot's `REPUTE` payload is preserved
  on round trip, but reputation changes are not modelled and a fresh save writes
  an empty faction scaffold.
- Multi-module save writing: the current module is rebuilt from live state;
  the other visited modules are preserved verbatim as their original saved
  ERFs, not re-simulated or updated.
- Full saved-object graph restoration beyond GIT list state (effects, action
  queues, combat-round data, non-GIT per-object locals).
- Animation/skinning/weapons, cutscene cameras, audio/EAX, Bink/lip sync, full
  `.gui`, minigames, localization, mods/Override precedence and P6 content.
- Delayed functor execution (`DelayCommand`/`AssignCommand`), which remains a
  dedicated VM sprint because it changes the `STORESTATE` contract pinned by the
  8,666-script parity gate.

## Evidence index

- `docs/evidence/s9/ctest.log`
- `docs/evidence/s9/original-save-load.log`
- `docs/evidence/s9/original-save-party.log`
- `docs/evidence/s9/original-save-roundtrip.log`
- `docs/evidence/s9/original-save-create.log`
- `docs/evidence/s9/original-save-party-roundtrip.log`
- `docs/evidence/s9/validate-roundtrip.log`, `validate-create.log`, `validate-party.log`
- `docs/evidence/s9/roundtrip-parse.json`, `create-parse.json`, `party-parse.json`
- `docs/evidence/s9/saves/000009 - ENGINE/`, `000010 - ENGINE/`
- `docs/evidence/s9/source-before.json`, `source-after.json`, `source-unchanged.log`
- `docs/evidence/s9/all-modules.log`, `all-modules.json`
- `docs/evidence/s9/vm-corpus.log`, `vm-parity.json`
- `docs/evidence/s9/parity.log`, `parity-gl.png`, `parity-vk.png`
- `docs/evidence/s9/tools-tests.log`
- `docs/evidence/s9/install-sentinel.log`
- Technical note: [docs/recon/47_ORIGINAL_SAVES.md](recon/47_ORIGINAL_SAVES.md)
