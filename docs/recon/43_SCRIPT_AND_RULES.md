# Recon 43 - native script runtime, dialogue conditions, and P5 rules core

Status: Sprint 6 evidence note. This note records what the engine now does and how each claim was
measured. All game data comes from the user's read-only KotOR 1.0.3.0 install
(`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`).

## 1. Native NCS VM

`engine/src/script/Ncs.cpp` decodes the documented NCS V1.0 wire form: 13-byte header
(`NCS V1.0`, `0x42`, big-endian size) and the opcode stream `0x01-0x2D`. The decoder keeps byte
offsets, qualifiers, jump operands, action id/argc, constants, `DESTRUCT` triples and
`STORESTATE` operands. `validateControlFlow` checks every JMP/JSR/JZ/JNZ and nonzero
`STORESTATE` target against the instruction offsets.

`engine/src/script/Vm.cpp` implements the verified stack machine:

- typed cells (`int`, `float`, `string`, `object`, engine handles) and the separate JSR/RETN
  return stack with the recursion limit of 8;
- `SAVEBP` pushes the old BP and sets `BP = SP-1`, the value verified against the live engine in
  Sprint 3; `RESTOREBP` restores it;
- `CPTOPSP`/`CPDOWNSP`/`CPTOPBP`/`CPDOWNBP`, `MOVSP`, `INCSP`/`DECSP`, `INCBP`/`DECBP`,
  `DESTRUCT`, `STORESTATE`/`STORESTATEALL`, and all arithmetic/logic/comparison opcodes;
- the oracle's numeric rules: truncating integer division, zero divisor -> 0, 32-bit masks for
  shifts/complement, case-insensitive string equality/order, and 3-cell vector comparisons.

Action mediation has two modes. Typed-default mode (no sink) consumes the declared arguments and
pushes the declared return default, with `ActionWait` (202) blocking. Functional mode calls
`ActionRuntime`.

Verification (`engine/scripts/check_vm_corpus.py`, report `docs/evidence/s6/vm-parity.json`):

| Run | Files | Status | Mismatches |
|---|---:|---|---:|
| typed-default vs regenerated Sprint 6 oracle | 8,666 | 8,184 returned / 472 blocked / 10 budget | **0** |
| functional vs regenerated Sprint 6 oracle | 8,666 | 8,185 returned / 472 blocked / 9 budget | **0** |

The compared fields are status, steps, max stack, action counts, top-of-stack, invalid jump targets,
instruction count, error/error_ip, blocked action, and (functional) runtime specific/generic/default
sets, timers, queued actions and world counters. The functional run has 303,616 ACTION calls.

The Sprint 2 default oracle was generated before the verified `SAVEBP = SP-1` fix and is stale for
scripts that depend on globals; Sprint 6 regenerated it at
`artifacts/lanes/s6-01-vm/default_oracle_s6.json`. The functional oracle was regenerated after the
action-runtime fix below.

## 2. Action runtime

`engine/src/script/World.*` implements stable object ids, tags (nth lookup), areas, typed locals,
globals, party, inventory, cursors, action queue, delayed timers, journal, world attrs and summary
counters. `engine/src/script/ActionRuntime.*` ports the verified Sprint 3 action model:

- specific handlers for object/tag/area/inventory/global/local/math/string/vector/journal/queue
  actions;
- `behaviors` query resolution for the corpus-used getters (object fields, faction queries,
  locations, time-of-day, nearest object, attackers, etc.);
- `SWMG_*` state cells; all other actions resolve by the same name-prefix rules as the oracle;
- `ActionWait`, `DelayCommand`, `AssignCommand`, `ActionPauseConversation`, and `SetListenPattern`
  preserve queue/timer counts.

`engine/src/script/Random.cpp` reproduces CPython's MT19937 seeding (`init_by_array` over the
32-bit limbs of `0x4B4F544F52`) and `randrange`/`randint` so `Random`, `dN` and `RandomName` match
the Python oracle.

`tools/analysis/gen_cpp_action_table.py` generates `engine/src/script/ActionTable.inc` from the
verified 772-action spec (ids 0..771, names, return kinds, parameter kinds). The generated table is
committed; the engine does not read the artifact at runtime.

Tool fault fixed: `tools/vm/actions_runtime.py` `_swmg` returned no cells for
`SWMG_AdjustFollowerHitPoints` even though its declared return is `int`. That caused two shipped
scripts to underflow when the C++/Python functional VM ran with the corrected BP. `_swmg` now
returns the declared default for Set/Adjust, and `tools/vm/tests/test_action_runtime.py` has a
regression case.

## 3. Engine hooks and event objects

`kotor::Entity` and `kotor::Scene` now carry a hook map keyed by GFF field name. `Entities.cpp`
collects the creature/door/placeable/trigger/store hooks (`ScriptSpawn`, `ScriptHeartbeat`,
`ScriptDialogue`, `ScriptDeath`, `OnOpen`, `OnUsed`, `ScriptOnEnter`, ...) from the template and
then applies non-empty GIT instance overrides. `Scene.cpp` collects ARE hooks (`OnEnter`, `OnExit`,
`OnHeartbeat`, `OnUserDefined`) and IFO module hooks (`Mod_OnModLoad`, `Mod_OnModStart`,
`Mod_OnHeartbeat`, `Mod_OnPlrDeath`, ...).

`GameSession::initScriptWorld` assigns one stable script object per GIT entity (plus the PC),
copies positions/HP/AC/CR, preserves globals/party/journal across module loads, and runs:

- IFO `Mod_OnModLoad`/`Mod_OnModStart`;
- each entity's `ScriptSpawn` once;
- ARE `OnEnter` with the PC as `GetEnteringObject`.

Runtime execution points: `ScriptHeartbeat`/`OnHeartbeat` every 6 simulated seconds; trigger
`ScriptOnEnter`/`ScriptOnExit`; door `OnOpen`/`OnClick`; placeable `OnUsed`; creature `ScriptDeath`
on a kill; and the module `Mod_OnPlrDying`/`Mod_OnPlrDeath` hooks. Actions that read event objects
(`GetEnteringObject`, `GetExitingObject`, `GetLastSpeaker`, `GetClickingObject`, `GetLastUsedBy`,
`GetItemActivated`, `GetLastKiller`, ...) read the per-run context.

Measured on `end_m01aa`: **284 hooks on 54 entities** plus 3 scene hooks; the load executes 25
shipped scripts (`k_hen_spawn01`, `k_pdan_trask_9`, `k_def_spawn01`, `k_pend_room3_02`,
`k_pend_sith_9`, `k_pend_weld02`, `k_pend_bridge01`, `k_pend_area01`, ...). The trigger, heartbeat,
area branch and death hook each ran in `--script-test` (`docs/evidence/s6/script.log`).

## 4. Dialogue conditions

KotOR 1 carries entry conditions in `StartingList[i].Active`, not in the entry/reply structs. The
DLG parser now retains `startingActive` in File order and also reads `Active` on entries/replies if
a mod supplies it. `startDialogue` runs each starting condition through the native VM and selects
the first truthy entry; an empty `Active` is unconditional. Entry and reply `Script` hooks run once
per node, and reply `Quest` is retained for combat/journal handling.

On the real `end_trask01`, the first candidate is `k_pend_trask00` over entry 54 and returns 1. The
log records `dialogue start end_trask01 entry=54` and `dialogue condition k_pend_trask00 entry=54 =
1` (`docs/evidence/s6/script.log`). Control entries (empty text, one continuation) are executed and
then auto-advanced, so the player sees the first spoken line rather than a blank control node.

## 5. P5 rules core

`engine/src/game/Rules.cpp` loads the shipped tables from the install: `classes`, `cls_atk_1/2/3`,
`cls_st_*`, `exptable`, `skills`, `feat`, `baseitems`, `featgain`.

- Character attributes come from the class row; level 1 Soldier has STR 16, DEX 14, CON 14, INT 10.
- Hit die, skill-point base, feat table and attack/save table names come from `classes.2da`.
- `cls_atk_*` and `cls_st_*` rows are indexed by `level-1` (the row labels are 0-based while the
  `level` column is 1-based); this was a real bug found by `--rules-test`.
- `exptable` row `level=2, xp=1000` is the level-up threshold; the `0xFFFFFFFF` sentinel on the
  last row is parsed as a hex integer and rejected as an int32 threshold.
- `baseitems.equipableslots` is a hexadecimal mask; the lowest set bit is the equipment slot
  (Long Sword `0x00030` -> slot 4).
- AC = 10 + DEX modifier + equipped `baseac`; attack = BAB + STR modifier; weapon dice come from
  the equipped `numdice`/`dietoroll`.
- Combat uses 6-second rounds, initiative `d20 + DEX modifier`, natural-1 automatic miss,
  natural-20 automatic hit and critical confirmation, then damage dice and death.
- `exptable` also drives level-up; the PC gets the full award and the sheet is recomputed.

Measured (`docs/evidence/s6/rules.log`): Soldier hp=12, BAB=1, fort=2, skill points=2, feat points=1,
AC=12, attack=+4, level(999)=1, level(1000)=2, xp(2)=1000, slot(2)=4; two deterministic d20 rolls
(18 hit / 5 miss) from seed `0x12345678`.

## 6. Party, inventory, journal, save

`GameSession` owns the character sheet, party tag roster, inventory items (resref, base item, cost,
stack, slot, equipped), gold and journal map. Script actions route through the `ActionHost`:

- `GiveXPToCreature`/`SetXP`/`GetXP`, `GiveGoldToCreature`/`TakeGoldFromCreature`/`GetGold`;
- `GiveItem`/`CreateItemOnObject`/`ActionGiveItem`/`ActionTakeItem`;
- `AddJournalQuestEntry`/`RemoveJournalQuestEntry`/`GetJournalEntry`;
- `AddPartyMember`/`RemovePartyMember`/`AddToParty`/`RemoveFromParty`/`IsObjectPartyMember`;
- `BeginConversation`/`ActionStartConversation`, `StartNewModule`, `DestroyObject`, `OpenStore`.

Engine save schema `kotor.engine.player/2` stores module/pose/HP, the character sheet, journal,
party, inventory and script globals/locals. `--systems-test` proves journal add/remove, script
journal/XP/gold, item add/equip/remove/readd, party join/leave/rejoin, a shipped inventory script,
and a full save/load round-trip including a local (`docs/evidence/s6/systems.log`).

## 7. Open items

- Merchants/stores, encounters and spawning tables, minigames, cutscenes, movies/audio and the full
  `.gui` set are Sprint 7.
- Original KotOR save compatibility (SAVEGAME.sav) is not implemented; the engine slot is local.
- Feat/Force-power selection UI is not present; validation and sheet state are.
- Delayed functors (`DelayCommand`/`AssignCommand`) are scheduled and counted but not executed;
  this matches the functional oracle, which mediates them.
- Scripts in other modules are available only when that module is bound (the engine's VFS is
  module-scoped); the corpus gate reads the extracted unique corpus directly.
