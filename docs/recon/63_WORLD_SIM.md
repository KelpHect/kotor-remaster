# World simulation layer: audit findings and implementation contract

Status: **in progress.** This replaces the prototype script host. It records why the
Sprint 0-14 gates passed while the game could not be completed, and the contract every
action family is written against.

## 1. Audit conclusion

The Sprint 0-14 gates prove that data parses, scripts decode and run to completion, and
modules load. They do not prove that scripts change the world.

Measured from `ActionTable.inc`, the host switch and the recorded VM corpus: of **344**
actions used by shipped scripts, **220** had no effect: 200 returned a typed default and
20 `Action*` were queued and never executed.

Blocking defects, each confirmed in code:

| # | Defect | Effect on play |
|---|--------|----------------|
| D1 | `ActionRuntime::generic` answers any prefix-matched name with a default | "0 unclassified" is true by construction; quest checks read 0 |
| D2 | Queued `Action*` entries never run and accumulate | No scripted NPC movement, doors, jumps or equips |
| D3 | DLG link `Active` is never parsed; replies always take `entries.front()` | Every condition-gated branch is treated as open |
| D4 | `EndConversation*`, `Quest`, `QuestEntry`, `PlotIndex`, `PlotXPPercentage` are ignored; `global.jrl` is unread | Journal and plot XP do not advance from dialogue |
| D5 | No per-module state; every transition rebuilds the world | Kills, loot and locals reset on return |
| D6 | Saves hold no per-object state, timers or world time | Save/load loses world progress |
| D7 | Locks, keys, traps, Plot and Min1HP are not parsed; doors do not block | Sequence breaks; quest NPCs can be killed |
| D8 | Most event hooks never fire; `SignalEvent`/UserDefined are dead | The shipped AI and plot event handlers never run |
| D9 | Only the player has combat, with invented NPC stats and hard-coded XP | Encounters are not the shipped encounters |
| D10 | Encounters spawn at load, not on entry | Wrong pacing and counts |
| D11 | Party actions are stubs; companion sheets are hard-coded | Recruitment and party changes fail |
| D12 | `OpenStore`, transition triggers and the `StartNewModule` waypoint are ignored | Merchants and many area changes fail |
| D13 | Effects are not modelled for script use | `ApplyEffectToObject` and all `Effect*` are inert |

Gate defects to correct: the retail differential passes with an empty retail trace (81 of
108 module reports) and unions the corpus into "executed"; `check_retail_traces.py` tests
table presence; five of seven bug-parity cases test pure helpers rather than the engine;
the dialogue matrix never asserts outcomes; `kotor_taris_transition` duplicates
`kotor_door_transition`; artifact-gated tests vanish instead of reporting as skipped.

## 2. Boundaries

- Game content is loaded from the user's install at runtime. Never commit or embed game
  assets, dialogue text or decompiled code. Tags, resrefs and numeric indices are allowed
  in test fixtures where needed to identify an object.
- Behaviour must cite evidence. Use `tools/analysis/re_lookup.py action <name>` and
  `artifacts/game-extract/nwscript.nss`. Where the binary is ambiguous, say so in a
  comment and in the family's report rather than inventing a rule.
- `ActionRuntime` with no host stays as is: the VM corpus regression oracle depends on it.
  Live behaviour goes in the host layer.

## 3. Architecture

`GameSession::action` first consults a 772-entry handler table:

```cpp
// src/game/ScriptHost.h
struct ActionCall {            // typed argument access, return helpers, world access
    GameSession& session;
    script::World& world;
    script::Vm& vm;
    const Args& args;
    std::uint16_t id;
};
using ActionHandler = std::optional<std::vector<script::Value>> (*)(ActionCall&);
void registerObjectActions(HandlerTable&);   // one register function per family file
```

Each family lives in its own file and is the only file its owner edits:

| File | Family |
|------|--------|
| `ScriptObjects.cpp` | Object queries, locations, tags, area iteration, stats |
| `ScriptEffects.cpp` | `Effect*`, apply/remove, effect iteration |
| `ScriptActions.cpp` | Per-object action queue and `Action*` |
| `ScriptItems.cpp` | Item ownership and inventory iteration |
| `ScriptParty.cpp` | Party table, available NPCs, leader |
| `ScriptCombat.cpp` | Talents, combat queries, AI support |
| `ScriptWorldState.cpp` | Doors, locks, traps, plot flags, factions, map, tokens, UI and cinematic |
| `ScriptEvents.cpp` | `SignalEvent`, event constructors, hook dispatch |

Shared state:

- **Engine values:** `Value::makeEngine(id)` indexes an `EngineStore` holding locations,
  effects, events and talents. Id 0 means invalid.
- **Actors:** the PC and every creature are actors with a position, facing and action
  queue. The PC's position remains `position_`; an accessor hides the difference.
- **Events:** queued with `{target, type, userDefinedNumber, delay}` and dispatched to the
  matching hook with the correct `OBJECT_SELF` and context objects.
- **Module state:** `std::map<module, ModuleSnapshot>` is captured on leave, reapplied on
  re-entry and persisted in saves.

## 4. Done criteria

The gate is `kotor_quest_playthrough`: a bot using only player-level inputs (move, talk,
choose, use, attack, equip, level up) that starts a new game and reaches the end of the
game, asserting journal and plot globals at each milestone. A teleport or a direct state
write fails the gate unless listed, with a reason, in the final report.
