# Sprint 8 - closeout of the remaining tool-reachable engine work

Status: **complete - gate S8 met (2026-11-03)**. This sprint closes the open items that were
concrete engine work rather than new content systems: every shipped module is now swept through the
C++ loader, encounter reset/respawn timers run, companions follow and the leader can be selected,
and feats/Force powers can be selected from the character panel. The remaining open list is only
the genuinely future P5/P6 work (original saves, animation, cutscene cameras, audio/movies,
minigames, full `.gui`, mods and content sweeps).

Baseline: Sprint 7 / gate S7 (`docs/SPRINT7_REPORT.md`): 21/21 CTest, stores, encounters,
companions, systems UI and save schema 3.

## Gate S8

1. **All 117 modules load** - a sweep launches the C++ loader for every module RIM in the install.
   All 117 exit 0, name the requested area, load with **zero model warnings**, and report their
   scene counts. No module is skipped.
2. **Encounter reset/respawn** - UTE `Reset`, `Respawns` (`0` = none, `-1` = unlimited) and
   `ResetTime` are simulated. When every spawned creature is dead the timer runs; a permitted
   respawn creates a fresh group at the same spawn points and re-runs `OnEntered`. A real
   `tar_m03ab` encounter respawns after 30 s.
3. **Companion formation and leader** - companions follow the leader in two-column formation,
   respect wall/floor movement, and the party leader can be switched between PC and a companion.
   The party panel exposes selection; the gate measures a follower closing from 8 m to 5.5 m in
   6 s and checks the leader tag.
4. **Feat/Force-power selection** - `feat.2da`/`spells.2da` validation and point spending are
   exposed through the character panel. A level-1 Soldier can learn `TWO_WEAPON_FIGHTING` and
   `FORCE_POWER_AFFECT_MIND`; duplicates and unmet prerequisites are refused.
5. **Regression** - 23/23 CTest targets pass, including the full 117-module sweep; the VM corpus
   is still 8,666/8,666 with 0 mismatches, GL/Vulkan scene parity is 0.03/255, the Python suite is
   143 tests with 7 skips, and the install sentinel is unchanged.
6. **Evidence and docs** - report, technical note, logs and the all-module JSON are committed.

## Non-goals (still genuinely future)

- Original KotOR `SAVEGAME.sav` compatibility (load and write).
- Creature animation/skinning, weapon attachment, cutscene camera playback.
- Audio/EAX, Bink movies, lip sync and the full `.gui` screen set.
- Delayed functor execution (`DelayCommand`/`AssignCommand`) and merchant store-gold/identify/
  reputation depth.
- Minigames (pazaak, swoop, turret), encounter content sweeps, all dialogue/quest branches,
  localization, mods/Override runtime precedence and the P6 content checklist.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s8-01-sweep | all-module loader sweep, emitter-model warning cleanup | `engine/scripts/sweep_modules.py`, `engine/src/kotor/Entities.cpp` |
| s8-02-encounter | reset/respawn timers and gate | `engine/src/game/GameSession.*` |
| s8-03-party | formation, leader switching, gate | `engine/src/game/GameSession.*` |
| s8-04-feats | feat/power validation and panel controls | `engine/src/game/Rules.*`, `engine/src/app/Application.cpp` |
| s8-05-gate | CTest, evidence, report | `engine/CMakeLists.txt`, `docs/evidence/s8/` |
| s8-06-report | report and technical note | `docs/SPRINT8_REPORT.md`, `docs/recon/45_CLOSEOUT.md` |

## Running the closeout gates

```powershell
cd engine
cmd /c "scripts\build.bat msvc-release"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $ctest --preset msvc-release --output-on-failure
```

Individual proofs:

```powershell
.\build\msvc-release\kotor_engine.exe --module tar_m03ab --encounter-reset-test --null --headless
.\build\msvc-release\kotor_engine.exe --module end_m01aa --party-test --null --headless
.\build\msvc-release\kotor_engine.exe --module end_m01aa --rules-test --null --headless
python ..\engine\scripts\sweep_modules.py --exe .\build\msvc-release\kotor_engine.exe `
  --install E:/SteamLibrary/steamapps/common/swkotor `
  --out ..\docs\evidence\s8\all-modules.json
```
