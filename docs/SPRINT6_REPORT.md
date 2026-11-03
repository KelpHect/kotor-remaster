# Sprint 6 report - P5 script runtime and systems core

Status: **complete - gate S6 met (2026-11-03)**. The engine now runs the shipped NWScript corpus on
its own C++ VM, executes the module's real script hooks, selects dialogue entries from their
`StartingList.Active` conditions, and owns a 2DA-driven character sheet with d20 combat rounds,
party, inventory, journal and XP state. All numbers below are from the final run; the evidence
folder is [docs/evidence/s6](../evidence/s6/).

## Environment

- Install: `E:/SteamLibrary/steamapps/common/swkotor`
- Build: `swkotor.exe` SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
- Compiler: MSVC 19.51.36231 (C++23), CMake preset `msvc-release`
- GPU: AMD Radeon RX 9070 XT (unchanged from Sprint 5)
- Build command: `cmd /c "engine\scripts\build.bat msvc-release"`
- Test command: `ctest --preset msvc-release --output-on-failure`

## 1. Build and test result

```text
17/17 Test #17: kotor_vm_corpus ..................   Passed    2.25 sec

100% tests passed, 0 tests failed out of 17
Total Test time (real) =   4.92 sec
```

The 13 Sprint 5 targets still pass; four new targets were added: `kotor_script`, `kotor_rules`,
`kotor_systems` and `kotor_vm_corpus`. See `docs/evidence/s6/ctest.log`.

The Python research suite also stays green: **143 tests, 0 failures, 7 skips**
(`docs/evidence/s6/tools-tests.log`), including the new SWMG return regression.

## 2. Native NCS VM and corpus parity

`engine/src/script/` contains the decoder, VM, generated action table, world, action runtime,
Python-compatible MT19937 and the corpus runner. `kotor_vm_corpus` runs both modes against the
regenerated oracles:

```text
default: files=8666 oracle=8666 mismatches=0
functional: files=8666 statuses={'returned': 8185, 'blocked': 472, 'budget': 9} action_calls=303616 mismatches=0
report: docs\evidence\s6\vm-parity.json
```

The typed-default oracle has 8,184 returned / 472 blocked / 10 budget; the functional oracle has
8,185 returned / 472 blocked / 9 budget. The compared fields are listed in
`docs/recon/43_SCRIPT_AND_RULES.md`. Full-corpus functional totals are **303,616 ACTION calls**,
matching Sprint 3.

The VM/action runtime is also unit-tested in `kotor_formats`: a hand-built CONST/ADD/RETN program
returns 3 in 4 steps, control-flow validation is clean, and the MT19937 stream matches CPython
(`21, 97, 17, 49, 86` for `randrange(100)` and `3, 3, 7, 7, 4` for `1d8`).

## 3. Shipped script hooks

`end_m01aa` loads **284 hooks on 54 entities** plus 3 scene hooks (ARE `OnEnter`, `OnUserDefined`,
IFO `Mod_OnActvtItem`). The load executes 25 scripts, including the Trask spawn, room spawns, Sith
spawns, bridge and area enter scripts.

`kotor_script` output:

```text
script test: hooks=284 entities=54 scene_hooks=3 runs=25
script test: hooks_loaded=1
script test: trask_spawn_ran=1
script test: area_enter_ran=1
script test: creature_spawn_ran=1
script test: trigger_script_ran=1
script test: heartbeat_hook_ran=1
script test: door_hook_ran=1
script test: door_hook_logged=1
script test: door_script_ran=1
script test: pc_branch_ran=1
script test: hostile_died=1
script test: death_hook_ran=1
script test: dialogue_condition_ran=1
script test: dialogue_selected_54=1
script test: dialogue start end_trask01 entry=54
script test: PASS
```

The test advances one 6-second heartbeat, enters the shipped `end_trig02`, invokes the real
`end_door16` `OnOpen` script (`k_pend_door16`), kills a hostile to run its `ScriptDeath`, and checks
the area branch with the PC as `GetEnteringObject`. Placeable `OnUsed` is exercised by the
playthrough's `end_locker01` interaction. See `docs/evidence/s6/script.log`.

## 4. Dialogue conditions

The DLG parser now reads `StartingList.Active` (and entry/reply `Active` if a mod supplies it).
`end_trask01` has 19 conditional starting entries. The VM runs `k_pend_trask00` first; it returns 1
and entry 54 is selected. Empty control entries are executed and auto-advanced, so the visible
conversation starts on Trask's first spoken line. Reply/entry scripts execute once per node.

```text
script: script k_pend_trask00 status=returned steps=15 actions=580:1 top=1
script: dialogue condition k_pend_trask00 entry=54 = 1
script: script k_pend_traskdl40 status=returned steps=22 actions=6:1,22:1,200:2,205:1,206:1
script: dialogue start end_trask01 entry=54
dialogue test: lines=9
```

`docs/evidence/s6/dialogue.log` contains the full branch transcript.

## 5. Character sheet and d20 rounds

Rules are loaded from `classes`, `cls_atk_1/2/3`, `cls_st_*`, `exptable`, `skills`, `feat`,
`baseitems` and `featgain`. The level-1 Soldier check:

```text
rules test: level(999)=1 level(1000)=2 xp(2)=1000
rules test: soldier hp=12 bab=1 fort=2 skills=2 feats=1 ac=12 attack=4
rules test: hit_die_10=1
rules test: bab_1=1
rules test: fort_2=1
rules test: hp_12=1
rules test: skill_points_2=1
rules test: feat_points_1=1
rules test: level_0=1
rules test: level_999=1
rules test: level_1000=1
rules test: xp_level_2=1
rules test: long_sword_slot_4=1
rules test: ac_12=1
rules test: attack_4=1
rules test: feat_prereq_refused=1
rules test: attack1 roll=18 total=22 hit=1 crit=0 damage=6
rules test: attack2 roll=5 total=9 hit=0 damage=0
rules test: PASS
```

Combat now uses 6-second rounds and initiative; the real hostile dies in one round:

```text
combat test: end_darkjedi01 dead=1 hp=-3/8 player=7/12 steps=361 rounds=1
```

The 1800-second playthrough still passes through the real dialogue, trigger, placeable, combat,
door transition and save/load:

```text
playthrough: dialogue lines=9
playthrough: trigger end_trig02 fired=1
playthrough: placeable end_locker01 used=1 logged=1
playthrough: combat target end_darkjedi01 dead=1 playerHp=7/12 steps=361
playthrough: transition end_m01aa -> end_m01ab ok=1
playthrough: save/load module=end_m01ab same=1
playthrough: simulated 1800.01 s, events=32
playthrough: PASS
```

## 6. Party, inventory, journal, save

```text
systems test: journal 0 -> 1
systems test: script_journal=1
systems test: script_journal_state=1
systems test: script_xp=1
systems test: gold 0 -> 200
systems test: script_gold=1
systems test: level_up=1
systems test: xp=1015 level=2 hp=24/24
systems test: item_added=1
systems test: item_equipped=1
systems test: equipped_state=1
systems test: item_removed=1
systems test: party_joined=1
systems test: party_left=1
systems test: party_rejoined=1
systems test: action_script_ran=1
systems test: save_written=1
systems test: save_loaded=1
systems test: xp_roundtrip=1
systems test: level_roundtrip=1
systems test: inventory_roundtrip=1
systems test: party_roundtrip=1
systems test: local_roundtrip=1
systems test: PASS
```

`k_pend_traskdl50` is a shipped action script and creates items/writes a global on the real world.
Save schema `kotor.engine.player/2` covers the character, journal, party, inventory and script
globals/locals. See `docs/evidence/s6/systems.log`.

### 6.1 Rendering regression check

The script work is presentation-neutral; both backends still render the same scene:

```text
mean_abs_channel_diff=0.0300
rms_channel_diff=1.8755
threshold=1.0000
verdict=PASS
```

Captures: `docs/evidence/s6/parity-gl.png`, `docs/evidence/s6/parity-vk.png`,
`docs/evidence/s6/parity-report.txt`.

## 7. Install safety
A full SHA-256 sentinel of the 15,380-file / 3.36 GiB install was taken before the final gate and
compared after the 17-test CTest run and the Python suite:

```text
{"files": 15380, "manifest_drift": 2, "pass": true,
 "session": {"changed": [], "changed_count": 0, "extra": [], "extra_count": 0,
             "missing": [], "missing_count": 0, "unchanged": true}}
```

The two manifest drifts are `swkotor.ini` and `saves/steam_autocloud.vdf`, both written on
2026-10-26 and 2026-10-23 (before Sprint 6 started) by the earlier retail/live runs. Sprint 6 added,
removed and changed nothing. Evidence: `docs/evidence/s6/install-sentinel.log`,
`artifacts/lanes/s6-07-install/post.json`.

## 8. Faults fixed in our own tools

1. **`ActionRuntime._swmg` return width** - `SWMG_AdjustFollowerHitPoints` declares `int` but the
   Python functional runtime returned no cells. With the verified `SAVEBP = SP-1` semantics this
   underflowed two shipped scripts (`accelpad`). `_swmg` now returns the declared default for
   Set/Adjust; `tools/vm/tests/test_action_runtime.py` covers it. The functional corpus was
   regenerated and the C++ runtime matches it exactly.
2. **Stale Sprint 2 default oracle** - it predates the verified `SAVEBP` fix. Sprint 6 regenerated
   `artifacts/lanes/s6-01-vm/default_oracle_s6.json`; the engine matches it 8,666/8,666.
3. **Rules row indexing** - `cls_atk_*`, `cls_st_*`, `featgain` and `exptable` use 0-based row
   labels with 1-based `level` values. The first implementation indexed by level and produced
   BAB 2/Fort 3 at level 1; `--rules-test` caught it and the tables are now indexed by `level-1`
   (with `exptable` searched by its `level` column).
4. **`0xFFFFFFFF` sentinel** - the last `exptable` row uses a hex sentinel; base-10 parsing read it
   as 0 and made every XP value level 21. Parsing is now base-0 with an int32 range check.
5. **`GetIsPC`/`IsObjectPartyMember` on the PC** - the area and trigger scripts take player
   branches only when these return 1. The engine host now binds the PC explicitly.
6. **`check_parity.py` relative `--out`** - the capture path was passed to the engine unchanged
   while the engine ran with `cwd=engine`, so a relative `--out` wrote into `engine/engine/...` and
   the checker reported a missing capture. The checker now resolves `--exe` and `--out` first; the
   rendering gate still reports 0.03/255.

## 9. Limitations and Sprint 7

- Merchants/stores, encounters/spawn tables, minigames, cutscene cameras, movies/audio and the full
  `.gui` set are not implemented.
- Original KotOR save compatibility is not implemented; the engine save slot is local.
- Delayed functors (`DelayCommand`/`AssignCommand`) are scheduled and counted but not executed,
  matching the functional oracle's mediation.
- Feat/Force-power selection has no UI; validation and sheet state are present.
- Creature animation/skinning and weapon attachment remain Sprint 5 limitations.
- The door gate exercises `end_door16` `OnOpen`; other doors' scripts are loaded and use the same
  path but are not individually gated.
- Four modules are exercised (`end_m01aa`, `end_m01ab`, `tar_m02af`, `tar_m02aa`), not all 117.

## Evidence index

Committed copies are under [docs/evidence/s6](../evidence/s6/):

- `ctest.log` - 17/17 final CTest run
- `vm-corpus.log` - default and functional parity summary
- `vm-parity.json` - machine-readable parity report (0 mismatches)
- `script.log` - 284 hooks, 25 load runs, trigger/heartbeat/death/dialogue-condition proof
- `dialogue.log` - real `end_trask01` branch transcript
- `rules.log` - 2DA character sheet and d20 proofs
- `systems.log` - journal/XP/gold/inventory/party/save round-trip
- `combat.log` - 6-second round, real hostile death
- `playthrough.log` - 1800 s deterministic gate
- `parity-report.txt`, `parity-gl.png`, `parity-vk.png` - GL/Vulkan scene parity 0.03/255
- `tools-tests.log` - 143 Python tests, 7 skips
- `install-sentinel.log` - 15,380 files unchanged during the gate
- Technical note: [docs/recon/43_SCRIPT_AND_RULES.md](recon/43_SCRIPT_AND_RULES.md)
