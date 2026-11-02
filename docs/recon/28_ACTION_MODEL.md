# 28 — Functional action runtime and object/timer model (Sprint 3 / gate S3)

Status: **closed** — functional NCS runtime over the shipped corpus for build SHA-256
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## State model

`tools/vm/state.py` implements the headless world used by one script execution:

| surface | model |
|---|---|
| object identity | monotonically assigned `GameObject.id`; invalid is `0x7F000000` |
| tags | `World.tags[tag] -> [object ids]`; `GetObjectByTag(tag, nth)` / `GetWaypointByTag` |
| area | objects carry an area id (default `100`); `GetArea` returns it; `GetNextObjectInArea` iterates it |
| locals | `GameObject.locals[key]`; K1 numeric index is stored as its decimal string |
| globals | `World.globals[name]`; number/boolean/string variants share the name space per type |
| party | `World.party` (the PC is member 0); `GetFirstPC`, `GetPartyMemberByIndex` |
| inventory | `GameObject.inventory`; `GetNextItemInInventory`, `GetItemInSlot`, `GetBaseItemType` |
| action queue | `World.action_queue` records `(action id, args, time, source)` |
| timers | `World.timers` records `(due time, action id, args, source)`; `DelayCommand` schedules |
| time | deterministic `World.time`, advanced only by explicit calls |
| RNG | deterministic `random.Random(0x4B4F544F52)` for `Random` and `dN` |

`ActionWait` (id 202) marks the VM `blocked`; `DelayCommand` (7) schedules a timer; every
`Action*` without a specific handler queues a record. No wall-clock waiting is performed.

## Action coverage

- **772/772** action ids resolve to a runtime handler; the direct runtime dispatch check
  (`python tools/vm/dispatch_check.py --specs ... --runtime`) reports **772 dispatched, 0 errors**.
- **344** unique action ids are exercised by the corpus (the functional run's branch behavior).
- **44** actions have specific handlers:
  `ActionPauseConversation`, `ActionWait`, `AssignCommand`, `DelayCommand`, `FindSubString`,
  `FloatToInt`, `FloatToString`, `GetArea`, `GetBaseItemType`, `GetDistanceToObject`,
  `GetFirstPC`, `GetGlobalBoolean`, `GetGlobalLocation`, `GetGlobalNumber`, `GetGlobalString`,
  `GetGold`, `GetIsObjectValid`, `GetItemInSlot`, `GetLocalBoolean`, `GetLocalNumber`,
  `GetNextItemInInventory`, `GetNextObjectInArea`, `GetObjectByTag`, `GetPartyMemberByIndex`,
  `GetPosition`, `GetStringLength`, `GetStringRight`, `GetSubString`, `GetTag`,
  `GetWaypointByTag`, `IntToFloat`, `IntToString`, `Random`, `SetGlobalBoolean`,
  `SetGlobalLocation`, `SetGlobalNumber`, `SetGlobalString`, `SetListenPattern`,
  `SetLocalBoolean`, `SetLocalNumber`, `StringToInt`, `Vector`, `VectorNormalize`, `abs`.
- **300** further corpus actions use generic semantic groups: `Get*`/`Is*`/`Has*`/`Find*`
  returns the typed default; `Set*`/`Clear*`/`Stop*`/`Destroy*`/`Remove*`/`Apply*`/`Change*`
  mediate state changes; `Action*` queues the action; `Play*`/`Sound*`/`Music*`/`Speak*`/`Effect*`
  mediate engine effects; minigame (`SWMG_*`, `TUR_*`, `PAZ_*`), party/journal, creation,
  conversation, and movie actions are mediated and recorded.
- **0** corpus actions fall through to an unclassified default.

## Functional corpus run

Command:

```bash
python tools/vm/corpus_run.py \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --budget 200000 --runtime \
  --out-json artifacts/lanes/s3-03-corpus/functional_corpus.json \
  --out-doc docs/recon/29_FUNCTIONAL_RUNTIME.md
```

Result (`docs/recon/29_FUNCTIONAL_RUNTIME.md`):

| metric | value |
|---|---:|
| unique NCS | 8,666/8,666 |
| returned | 8,185 |
| blocked at `ActionWait` | 472 |
| budget stops | 9 |
| decode errors | 0 |
| VM faults | 0 |
| unknown actions | 0 |
| invalid jump targets | 0 |
| ACTION calls | 303,650 |
| specific handlers used | 44 |
| generic handlers used | 300 |
| unclassified/default actions | 0 |
| timers scheduled | 20,406 |
| queued actions | 5,401 |

The 9 budget stops are polling loops with no `ActionWait`; they are expected for scripts that
observe engine state.

## Top action calls

| id | action | calls |
|---:|---|---:|
| 168 | GetTag | 129,707 |
| 340 | GetNextItemInInventory | 45,351 |
| 200 | GetObjectByTag | 41,229 |
| 7 | DelayCommand | 20,406 |
| 397 | GetBaseItemType | 15,227 |
| 155 | GetItemInSlot | 7,734 |
| 42 | GetIsObjectValid | 3,995 |
| 580 | GetGlobalNumber | 2,283 |
| 679 | GetLocalBoolean | 2,162 |
| 6 | AssignCommand | 1,989 |
| 548 | GetFirstPC | 1,684 |
| 176 | SetListenPattern | 1,504 |
| 197 | GetWaypointByTag | 1,324 |
| 578 | GetGlobalBoolean | 1,287 |
| 581 | SetGlobalNumber | 1,180 |

## Scope

This is a headless conformance runtime. Engine-side systems (combat, dialogue resolution,
renderer, audio, module transitions) are mediated, not reproduced. The live
`ExecuteCode`/`RunCommand` differential and compiler-codegen residuals remain later P3 work.
