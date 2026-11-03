# Sprint 9 - Original save compatibility and game profiles

Status: **complete - gate S9 met (2026-11-03)**. Sprint 8.5 closed the inherited data-backed
systems. Sprint 9 adds the compatibility layer the remaining engine work needs: a first-class game
profile (KotOR 1 now, KotOR 2 detected and gated), and native read/write support for the original
`SAVEGAME.sav` container and its sidecars.

Baseline: Sprint 8.5 `24/24` CTest, 117/117 modules, 0 warnings, VM 8,666/8,666, scene parity
0.03/255. Sprint 9 result: `33/33` CTest and independent Python save validation **0 errors /
0 warnings** on the round-trip, fresh-create and party slots.

## Gate S9

1. **Game profile** - `ke::kotor::GameProfile` identifies KotOR 1 (`swkotor.exe`,
   `0290889e...`) and KotOR 2 (`swkotor2.exe`, `6a522e71...`) from the install, drives the
   executable/build check, and reports the supported capability set. A wrong executable or an
   unknown build fails with a precise error; K2 module loading is not claimed by this gate.
2. **Original save read** - the native reader opens a real retail save folder:
   - outer `SAVEGAME.sav` ERF (MOD V1.0),
   - one nested `<module>.sav` ERF per saved module,
   - nested `Module.ifo`, area `.are`, area `.git`,
   - `PARTYTABLE.res`, `GLOBALVARS.res`, `savenfo.res`,
   - outer `INVENTORY`, `REPUTE`, `AVAILNPC*` entries.
   It maps module, entry position/bearing, PC sheet, gold, journal, inventory, party and script
   globals into a live `GameSession`, and overlays saved GIT object state (position, HP, door
   state, placeable loot) onto the module loaded from the install.
3. **Original save write** - the native writer emits a complete original-format save slot from a
   live session: `SAVEGAME.sav` (MOD V1.0 with a nested module ERF plus preserved visited-module
   ERFs), `PARTYTABLE.res`, `GLOBALVARS.res`, `savenfo.res`, and `Screen.tga`. The round-trip and
   fresh-create outputs must pass the independent Python save validator with **0 errors and
   0 warnings**; the multi-module party slot must add no new error and only inherits the source's
   `Mod_Tag` warnings for visited modules it preserves verbatim. It must reload in this engine with
   the same module, position, gold, journal, inventory, party and globals.
4. **Round-trip** - a real retail quicksave (`000000 - QUICKSAVE`, module `tar_m02af`) loads,
   writes, validates and reloads; a fresh `end_m01aa` session also writes and reloads. No source
   save or game file is modified.
5. **Regression** - **33/33** CTest targets pass, the 117-module sweep stays 0 failures /
   0 warnings, VM corpus parity stays 8,666/8,666 with 0 mismatches, scene parity stays 0.03/255,
   the Python suite stays green (143 tests / 7 skips), and the install sentinel reports 0 session
   changes.
6. **Evidence and docs** - report, technical note, logs and generated save listings are committed.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s9-01-profile | `GameProfile`, K1/K2 detection, build checks | `engine/src/kotor/Profile.*`, `Install.*` |
| s9-02-gff-writer | mutable GFF tree + canonical V3.2 writer + ERF/MOD writer | `engine/src/kotor/GffWrite.*`, `Archive.*` |
| s9-03-save-read | outer/nested ERF, sidecars, PC/party/inventory/globals mapping | `engine/src/kotor/SaveGame.*`, `GameSession.*` |
| s9-04-save-write | IFO/GIT/ARE, PT/GVT/NFO/INV, slot writer | `engine/src/kotor/SaveGame.*` |
| s9-05-gate | CLI modes, round-trip gate, CTest, Python validator evidence | `engine/src/app/*`, `engine/CMakeLists.txt` |
| s9-06-report | report, recon note, regression evidence | `docs/SPRINT9_REPORT.md`, `docs/recon/47_ORIGINAL_SAVES.md` |

## Explicitly not this sprint

- KotOR 2 module/world loading. The K2 profile is detected and gated; the first K2 world gate is
  Sprint 10 because it needs K2-specific 2DA/model/action verification.
- Full saved-object graph restoration beyond the GIT lists above (effects, action queues, combat
  round data, per-object locals that are not in the GIT).
- Animation/skinning/weapons, cutscene cameras, audio/EAX, Bink/lip sync, full `.gui`, minigames,
  localization, mods/Override runtime precedence and the P6 content checklist.
- Delayed functor execution (`DelayCommand`/`AssignCommand`). This remains a dedicated VM sprint
  because it changes the `STORESTATE` contract that the 8,666-script parity is pinned to.

## Running the gate

```powershell
cd engine
cmd /c "scripts\build.bat msvc-release"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $ctest --preset msvc-release --output-on-failure
```

```powershell
# real retail save -> engine state
.\build\msvc-release\kotor_engine.exe --install "E:/SteamLibrary/steamapps/common/swkotor" `
  --original-save-load "..\artifacts\lanes\06-dynamic\profile\saves\000000 - QUICKSAVE" `
  --null --headless

# engine state -> original slot -> reload
.\build\msvc-release\kotor_engine.exe --install "E:/SteamLibrary/steamapps/common/swkotor" `
  --original-save-roundtrip "..\artifacts\lanes\06-dynamic\profile\saves\000000 - QUICKSAVE" `
  --original-save-out "..\engine\build\msvc-release\000009 - ENGINE" --null --headless

# fresh original slot, no source save
.\build\msvc-release\kotor_engine.exe --install "E:/SteamLibrary/steamapps/common/swkotor" `
  --module end_m01aa --original-save-create "..\engine\build\msvc-release\000010 - ENGINE" `
  --null --headless

# independent Python validation
python ..\tools\resources\kotor_save.py validate "..\engine\build\msvc-release\000010 - ENGINE"
```
