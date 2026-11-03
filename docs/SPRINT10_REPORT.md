# Sprint 10 report - VM/script closure and object context

Status: **complete - gate S10 met (2026-11-03)**. Sprint 9 closed original saves
and profiles. Sprint 10 closes the script-runtime gap behind delayed actions:
`DelayCommand` and `AssignCommand` closures are captured, scheduled/queued and
executed, nested closures work, `ActionWait` continuations resume, and live
scripts resolve `OBJECT_SELF` instead of silently dropping object id 0.

## 1. What changed

### Closure capture

KotOR compiles an action argument as an inline closure:

```text
STORESTATE
JMP  <after body>
<body>
RETN
after body:
CONST <delay>
ACTION DelayCommand
```

`Vm::step` now snapshots the decoded instructions, the body entry after the
skip-`JMP`, the typed stack and BP. `Vm::doAction` registers the closure through
`ActionSink::registerFunctor`; `ValueType::Functor` carries the registry id.

### Scheduling and queueing

`World` owns the functor registry. `Timer` and `QueuedAction` now carry a
functor id and target object. `GameSession::update`:

- advances `World::time`,
- executes due `DelayCommand` closures,
- drains up to 32 queued `AssignCommand` closures per tick,
- removes executed functor entries.

Nested `DelayCommand` inside a delayed closure schedules a second functor and
executes it at the correct time.

### `OBJECT_SELF`

Live `ActionRuntime` now maps object id 0 (`OBJECT_SELF`) to the current `self`
context for locals, object actions and `AssignCommand` targets. The pinned
corpus runner keeps the original opt-in-off behaviour, so the 8,666-script
oracle remains byte-for-byte comparable.

`AssignCommand(target, closure)` overrides the closure's `self` and `speaker`
with `target`; the gate writes local 3 on `Carth`, not on the player.

`ActionWait(delay)` captures `ip + 1` with the current stack/BP and schedules
it as a continuation; the live VM is marked blocked in the original script and
the continuation runs after the delay. Corpus mode keeps the original blocked
behaviour, so the 472 blocked-oracle scripts are unchanged.

## 2. Gate S10 evidence

Custom fixture (`engine/tests/fixtures/delay_test.nss` / `.ncs`, compiled with
the historical `nwnnsscomp -v129`):

```text
delayed test: immediate local0=1 local1=0 local2=0
delayed test: after_1s local1=0 local2=7 carth3=9 local4=0
delayed test: after_2.5s local1=42 local4=99 local6=2 timers=0 queued=129
delayed test: shipped timers_after_script=1 functors_after_script=4
delayed test: functor log functor AssignCommand status=returned steps=5
delayed test: functor log functor DelayCommand status=returned steps=13
delayed test: functor log functor AssignCommand status=returned steps=9
delayed test: functor log functor DelayCommand status=returned steps=5
delayed test: functor log functor ActionWait status=returned steps=6
delayed test: shipped log script k_pend_time01 status=returned steps=917
delayed test: shipped_k_pend_time01 ran=1 functor=1 -> PASS
```

This proves:

- immediate script action,
- `AssignCommand` on self,
- `AssignCommand` on `Carth` with target `OBJECT_SELF`,
- a 2 s `DelayCommand`,
- a nested 1 s + 1 s `DelayCommand`,
- an `ActionWait(1.0)` continuation that resumes and writes local 6,
- a real shipped script (`k_pend_time01`) scheduling and executing its 10 s
  user-defined event.

Evidence: `docs/evidence/s10/delayed.log`.

## 3. Regression

- **CTest: 34/34 passed** (39.80 s).
- **117/117 modules**, 0 failures, 0 warnings, 11,887 entities, 51 encounters.
- **VM corpus: 8,666/8,666** default and functional, 0 mismatches, 303,616
  ACTION calls (pinned oracle mode; live `OBJECT_SELF` is opt-in).
- **GL/Vulkan parity: 0.0300**, PASS (`attempts=1`).
- **Python tools: 143 tests, 7 skips**, 0 failures.
- **Install sentinel: 15,380 files, 0 session changes.**

Evidence: `docs/evidence/s10/ctest.log`, `all-modules.log`, `vm-corpus.log`,
`parity.log`, `tools-tests.log`, `install-sentinel.log`.

## 4. Not in Sprint 10

- The 472 corpus-blocked scripts remain blocked in the pinned oracle; live
  sessions resume `ActionWait`.
- Non-functor queued actions (`PopUpGUIPanel`, `ActionPauseConversation`, ...)
  remain UI state records.
- Serializing pending timers/closures in a save. Script globals and object
  locals still round-trip.
- Full behavioural effects for every action, complete combat/Force/alignment
  depth, animation/skinning, cutscenes, audio/EAX, Bink/lip sync, full `.gui`,
  minigames, localization, mods/Override, KotOR 2 world loading and P6 content.

## Evidence index

- `docs/evidence/s10/delayed.log`
- `docs/evidence/s10/ctest.log`
- `docs/evidence/s10/all-modules.log`, `all-modules.json`
- `docs/evidence/s10/vm-corpus.log`, `vm-parity.json`
- `docs/evidence/s10/parity.log`, `parity-gl.png`, `parity-vk.png`
- `docs/evidence/s10/tools-tests.log`
- `docs/evidence/s10/install-sentinel.log`
- Fixture: `engine/tests/fixtures/delay_test.nss`, `delay_test.ncs`
- Technical note: [docs/recon/48_FUNCTORS.md](recon/48_FUNCTORS.md)
