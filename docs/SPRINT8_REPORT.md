# Sprint 8 report - closeout of the remaining tool-reachable engine work

Status: **complete - gate S8 met (2026-11-03)**. This sprint closes every open engine item that
could be finished from the shipped data and existing tooling without starting a new content system:
the full 117-module loader sweep is clean, encounter reset/respawn timers run, companions follow in
formation and the leader can be selected, and feats/Force powers can be learned from the character
panel. The remaining list is the genuinely future P5/P6 work.

## Environment

- Install: `E:/SteamLibrary/steamapps/common/swkotor`
- Build: `swkotor.exe` SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
- Compiler: MSVC 19.51.36231 (C++23), CMake preset `msvc-release`
- GPU: AMD Radeon RX 9070 XT

## 1. Build and test result

```text
100% tests passed, 0 tests failed out of 23
Total Test time (real) =  36.90 sec
```

The 21 Sprint 7 targets still pass; `kotor_encounter_reset` and `kotor_all_modules` were added.
See `docs/evidence/s8/ctest.log`.

Python suite: **143 tests, 0 failures, 7 skips** (`docs/evidence/s8/tools-tests.log`).

## 2. All 117 modules

`engine/scripts/sweep_modules.py` launches the C++ loader for every module RIM (excluding the
`_s`/`_a` sidecars). It checks the process exit, the requested module header, the area and the scene
counts, and requires zero model warnings.

```text
{"count": 117, "encounters": 51, "entities": 11887, "failures": 0, "pass": true, "warnings": 0}
```

The previous 13 warnings were all emitter placeables (`PLC_Smoke`, `PLC_StmVentI`,
`PLC_DripWatr`, `PLC_BuzzBugs`, ...) whose MDL parses successfully but has no trimesh. A parsed
model with no renderable triangles is now a valid empty mesh rather than a warning; missing or
malformed MDL/MDX still warns. The full JSON is `docs/evidence/s8/all-modules.json`.

## 3. Encounter reset/respawn

UTE `Reset`, `Respawns` (`0` = none, `-1` = unlimited) and `ResetTime` are now simulated. When
every creature in a spawned group is dead, `updateEncounters` runs the timer; a permitted reset
spawns a fresh group at the same points and reruns `OnEntered`. `tar_m03ab` has a real encounter
with `Respawns=-1` and `ResetTime=30`.

```text
encounter reset test: volume=2 spawned=2 reset_time=30 respawns=-1
encounter reset test: spawned 2 -> 4 alive=1
encounter reset test: respawned=1
encounter reset test: respawn_alive=1
encounter reset test: PASS
```

`docs/evidence/s8/encounter-reset.log`.

## 4. Companion formation and leader

Companions follow the leader in a two-column formation, use the same collision and floor checks as
the player, and the party leader can be switched between PC and a companion. The party gate
measures Carth closing from 8 m to 5.5 m over 6 simulated seconds.

```text
party test: join=1
party test: companion_sheet=1
party test: companion xp=800
party test: companion_80_percent=1
party test: selected=1
party test: leader_selected=1
party test: leader_back_to_pc=1
party test: follower distance 8 -> 5.49538
party test: formation_follow=1
party test: save_written=1
party test: save_loaded=1
party test: leader_roundtrip=1
party test: companion_roundtrip=1
party test: companion_xp_roundtrip=1
party test: PASS
```

`docs/evidence/s8/party.log`.

## 5. Feat and Force-power selection

`feat.2da` and `spells.2da` validation is now exposed in the character panel (`Feat ID` and
`Power ID` with Learn buttons). Feats spend `featPoints`; unmet prerequisites, duplicates and
missing points are refused. The level-1 Soldier gate learns `TWO_WEAPON_FIGHTING` and
`FORCE_POWER_AFFECT_MIND`; both survive save/load (`feat_roundtrip`, `power_roundtrip`).

```text
rules test: learn_feat=1
rules test: feat_added=1
rules test: duplicate_feat_refused=1
rules test: learn_power=1
rules test: power_added=1
rules test: PASS
```

`docs/evidence/s8/rules.log`.

## 6. Regression evidence

- VM corpus: **8,666/8,666** default and functional, 0 mismatches, 303,616 ACTION calls
  (`docs/evidence/s8/vm-corpus.log`).
- GL/Vulkan scene parity: **0.03/255**, PASS (`docs/evidence/s8/parity.log`).
- Install sentinel: 15,380 files, **0 session changes** (`docs/evidence/s8/install-sentinel.log`).
- CTest: **23/23**.

## 7. What remains and why

These are new content/system tasks, not unfinished tool work:

- Original KotOR `SAVEGAME.sav` load/write compatibility.
- Creature animation/skinning, weapon attachment and cutscene camera playback.
- Audio/EAX, Bink movies, lip sync and the full `.gui` screen set.
- Delayed functors (`DelayCommand`/`AssignCommand`) are scheduled and counted but not executed;
  the functional VM oracle mediates them.
- Merchant store gold, identify prices, reputation haggling and full store UI depth.
- Minigames, encounter content sweeps, all dialogue/quest branches, localization and mods/Override
  runtime precedence.
- The P6 content checklist and all-area gameplay parity.

## Evidence index

- `docs/evidence/s8/ctest.log` - 23/23
- `docs/evidence/s8/all-modules.log`, `all-modules.json` - 117/117, 0 failures, 0 warnings
- `docs/evidence/s8/encounter-reset.log` - real 30 s respawn
- `docs/evidence/s8/party.log` - formation and leader
- `docs/evidence/s8/rules.log` - feat/power selection
- `docs/evidence/s8/ui.log` - systems panel state
- `docs/evidence/s8/vm-corpus.log`, `vm-parity.json`
- `docs/evidence/s8/parity.log`, `parity-gl.png`, `parity-vk.png`
- `docs/evidence/s8/tools-tests.log`
- `docs/evidence/s8/install-sentinel.log`
- Technical note: [docs/recon/45_CLOSEOUT.md](recon/45_CLOSEOUT.md)
