# Recon 48 - action closures, timers and OBJECT_SELF

Status: Sprint 10 evidence note. Target build: `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## 1. Bytecode shape

KotOR compiles an action argument as an inline closure:

```text
STORESTATE
JMP  over_the_body
<body>
RETN
over_the_body:
CONST <delay>          ; for DelayCommand
ACTION DelayCommand
```

`k_pend_time01` is the shipped reference:

```text
0x2b STORESTATE 16
0x35 JMP 32
0x3b CONST 100
0x41 ACTION GetUserDefinedEventNumber
0x46 CPTOPSP -8
0x4e ACTION SignalEvent
0x53 RETN
0x55 CONST 10.0
0x5b ACTION DelayCommand
```

The normal path continues at `0x55`; the body starts at `0x3b`. The pinned
corpus VM previously set `pendingFunctor_` but discarded the body.

## 2. Capture and execution

`engine/src/script/Functor.h` stores:

- the decoded instruction stream,
- the body entry index (after the skip-`JMP`),
- a snapshot of the typed stack and BP,
- the `ActionRuntime` context map,
- the action source (`DelayCommand`/`AssignCommand`).

`Vm::step` captures this at `STORESTATE`. `Vm::doAction` registers it through
`ActionSink::registerFunctor` when an `Action` parameter is consumed;
`ValueType::Functor` carries the registry id without occupying a stack cell.

`World` owns the registry and adds functor ids to `Timer` and `QueuedAction`.
`GameSession::update`:

1. advances `World::time` and collects due timers,
2. runs each due closure from its captured IP/stack/BP in a fresh `Vm`,
3. drains up to 32 queued `AssignCommand` closures,
4. removes executed functor ids and leaves non-functor queued actions alone.

Nested closures work because a executed closure is still a normal `Vm`, and a
nested `STORESTATE` registers another functor in the same world.

## 3. OBJECT_SELF

`ActionRuntime` previously treated object id 0 as a missing object. In NWScript
id 0 is `OBJECT_SELF`; `OBJECT_INVALID` is `0x7F000000`. Live sessions enable
`selfAware_`, which maps id 0 to the current `self` context in:

- `GetLocal*`/`SetLocal*`,
- object-returning and object-taking actions,
- `AssignCommand` targets,
- the live host-action object argument.

The corpus runner leaves `selfAware_` off so the verified Sprint 6 oracle
continues to describe its pinned model. That is why the 8,666-script parity
result is unchanged while live scripts gain correct self semantics.

`AssignCommand(target, closure)` overrides the closure's `self`/`speaker`
context with `target`; the test assigns local 3 on `Carth`, not on the player.

`ActionWait(delay)` uses the same continuation mechanism: the live runtime
captures `ip + 1` with the current stack/BP, schedules it as a functor and marks
the VM blocked. The scheduled continuation resumes the script after the delay.
The corpus runner keeps `continuations_` off, so its 472 blocked scripts are
unchanged.

## 4. Test fixture

`engine/tests/fixtures/delay_test.nss` is compiled with the historical
`nwnnsscomp` (`-v129`) to `delay_test.ncs` (committed so the test does not need
the compiler). It checks:

| Check | Expected |
|---|---|
| immediate `SetLocalNumber(self, 0, 1)` | local 0 = 1 |
| `AssignCommand(self, ... local 2 = 7)` | local 2 = 7 after one update |
| `AssignCommand(Carth, ... local 3 = 9)` | `Carth` local 3 = 9 |
| `DelayCommand(2.0, ... local 1 = 42)` | local 1 = 42 after ~2 s |
| nested `DelayCommand(1.0, DelayCommand(1.0, ... local 4 = 99))` | local 4 = 99 after ~2 s |
| `ActionWait(1.0); SetLocalNumber(self, 6, 2)` | local 6 = 2 after ~1 s |
| shipped `k_pend_time01` 10 s event | functor log `status=returned` |

Evidence: `docs/evidence/s10/delayed.log`.

## 5. Residual work

- The 472 corpus-blocked scripts remain deliberately blocked in the pinned
  oracle; only live sessions resume `ActionWait`.
- Non-functor queued actions (`PopUpGUIPanel`, `ActionPauseConversation`, ...)
  are still state records for the UI layer, not executed by this scheduler.
- Pending timers/closures are not serialized in a save; script globals and
  object locals still are.
- The full behavioural effect of every action, combat/Force/alignment depth,
  audio, animation, cutscenes, UI and KotOR 2 were delivered in later sprints;
  faction reputation and P6 content remain.
