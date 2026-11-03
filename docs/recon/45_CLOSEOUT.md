# Recon 45 - open-work closeout: module sweep, encounter reset, formation and feat selection

Status: Sprint 8 evidence note. The target is the user's read-only KotOR 1.0.3.0 install
(`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`).

## 1. All-module loader sweep

`engine/scripts/sweep_modules.py` enumerates every `modules/*.rim` stem that does not end in `_a`
or `_s` (117 modules), launches `kotor_engine --module <stem> --scene-stats --null --headless`,
and parses the header, area, counts and warnings. The report requires:

- 117 rows;
- every process exit code 0;
- the requested module in the header;
- zero model warnings.

Result: 117/117, 0 failures, 0 warnings, 11,887 entities and 51 encountered volumes. The full
JSON is `docs/evidence/s8/all-modules.json`.

The only previous warnings were emitter placeables whose MDL parses cleanly but contains no
trimesh (`PLC_Smoke`, `PLC_StmVentI`, `PLC_DripWatr`, `PLC_BuzzBugs`). `loadModel` now treats a
successfully parsed empty model as an empty mesh, not an error. Missing MDL/MDX and parse errors
still warn.

## 2. Encounter reset/respawn

`EncounterState` carries `resetTime`, `resetTimer`, `respawnsRemaining` and `exhausted`.
`spawnEncounters` seeds these from the UTE:

- `Respawns = 0` -> no reset;
- `Respawns = -1` -> unlimited;
- positive value -> finite count.

`updateEncounters(dt)` checks every entity in the group. While any is alive the timer is zeroed;
when all are dead it advances. At `ResetTime` it calls `spawnEncounterCreatures`, which repeats the
same deterministic creature/spawn-point selection, appends new entities/meshes/script objects, and
returns the count. `OnEntered` runs again for the encounter entity. A spawn failure marks the
state exhausted.

Test module: `tar_m03ab`, volume 2 (`g_blckvulgrou002`, `Reset=1`, `Respawns=-1`,
`ResetTime=30`). The gate kills the initial two, advances 48 simulated seconds, and observes
2 -> 4 spawned with a live creature.

## 3. Companion formation and leader

`GameSession::updateParty(dt)` runs after movement/combat. The anchor is the PC unless
`leaderTag_` names a companion. Followers (all companions except the leader) take two-column
offsets behind the anchor:

```
target = anchor - forward * (1.6 + row*1.1) + right * (+/-1.2)
```

Movement is clamped to 3.2 m/s, uses `wallBlocked` with a 0.25 m radius, and snaps to the walkmesh
floor through `floorHeight`. `selectCompanion(index)` sets both the selected companion and
`leaderTag_`; `setPartyLeader("PC")` returns control/anchor to the PC. The party panel exposes the
selection.

Test: Carth starts 8 m from the player; 6 simulated seconds close the distance to 5.49538 m.
`leader_selected=1`, `leader_back_to_pc=1`.

## 4. Feat and Force-power selection

`Rules` loads `feat.2da` and `spells.2da`. `canTakeFeat` checks level, STR/DEX/INT/WIS and
`prereqfeat1/2`. `canTakePower` requires a row with `usertype=1`, a name, and any `prerequisites` /
`masterspell` already in the sheet. `learnFeat` spends `featPoints`; duplicates and invalid feats
are refused. `learnPower` refuses duplicates and invalid rows. The character panel exposes
`Feat ID`/`Power ID` inputs and Learn buttons.

Level-1 Soldier test: `TWO_WEAPON_FIGHTING` (3) is learned and a duplicate is refused;
`FORCE_POWER_AFFECT_MIND` (6) is learned.

## 5. Regression

- VM corpus: 8,666/8,666 default and functional, 0 mismatches, 303,616 ACTION calls.
- GL/Vulkan parity: 0.03/255.
- Python suite: 143 tests, 7 skips.
- CTest: 23/23.
- Install: 15,380 files, 0 session changes.

## 6. Remaining future work

Original SAVEGAME.sav compatibility, animation/skinning/weapons, cutscene cameras, audio/EAX,
Bink movies/lip sync, the full `.gui` set, minigames, encounter content sweeps, all dialogue/quest
branches, localization, mods/Override runtime precedence and the P6 content checklist.
