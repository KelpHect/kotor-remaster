# Sprint 10 - VM/script closure and object context

Status: **complete - gate S10 met (2026-11-03)**. Sprint 9 closed original-save
compatibility and game profiles. Sprint 10 closes the script-runtime gap that
gates real content: action closures (`DelayCommand`/`AssignCommand`) are now
captured, scheduled, queued and executed, and live scripts resolve
`OBJECT_SELF` instead of silently ignoring object id 0.

Baseline: Sprint 9 `33/33` CTest, 117/117 modules, VM 8,666/8,666, scene parity
0.0300, Python 143 tests / 7 skips, install unchanged. Sprint 10 result:
`34/34` CTest, including the new delayed-functor gate.

Scope note: an earlier roadmap sketch grouped "full combat/Force/alignment
effects" with a possible Sprint 10. This written gate narrows Sprint 10 to the
VM/script closure lane and moves full combat/Force/alignment to Sprint 11.
Those systems are **not** claimed as part of this gate.

## Gate S10

1. **Closure capture** - `STORESTATE` snapshots the instruction stream, closure
   entry instruction, stack and BP. The closure body is entered after the
   compiler's skip-`JMP`, not at the `STORESTATE` itself.
2. **`DelayCommand`** - the functor id is registered in `World`, the timer is
   scheduled at `world.time + delay`, and the closure executes when
   `GameSession::update` advances world time. A shipped `k_pend_time01` script
   executes its delayed user-defined event.
3. **`AssignCommand`** - the closure is queued and executes on the next update.
   The target object becomes the closure's `OBJECT_SELF`; the test assigns a
   local on `Carth`, not on the player.
4. **`OBJECT_SELF` context** - live `ActionRuntime` maps object id 0 to the
   current `self` context, so `SetLocalNumber(OBJECT_SELF, ...)`,
   `GetLocalNumber` and target-aware `AssignCommand` work. The pinned corpus
   oracle keeps its original behaviour because that mode remains opt-in.
5. **`ActionWait` resumption** - a live script blocked by `ActionWait(delay)`
   captures its continuation at the following instruction and resumes after the
   delay; the pinned corpus remains blocked as before.
6. **World clock and queue** - `GameSession::update` advances `World::time`,
   runs due timers and drains at most 32 queued functors per tick. Pending
   closures are removed after execution.
7. **Regression** - **34/34** CTest (39.80 s), 117/117 modules with 0 failures /
   0 warnings, VM corpus 8,666/8,666 with 0 mismatches, scene parity 0.0300,
   Python 143 tests / 7 skips, install sentinel 0 session changes.
8. **Evidence and docs** - report, technical note, fixture/source and logs.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s10-01-functor | `Functor` value, `STORESTATE` capture, `Vm::setEntry` | `engine/src/script/Functor.h`, `Vm.*` |
| s10-02-schedule | `World` timer/queue functors, `ActionRuntime` handlers | `engine/src/script/World.*`, `ActionRuntime.*` |
| s10-03-context | live `OBJECT_SELF`, closure execution in `GameSession` | `engine/src/game/GameSession.*` |
| s10-04-gate | fixture compiler output, `--delayed-test`, CTest | `engine/tests/fixtures/`, `engine/src/app/` |
| s10-05-evidence | report, recon note, parity/module/install evidence | `docs/SPRINT10_REPORT.md`, `docs/recon/48_FUNCTORS.md` |

## Explicitly not this sprint

- The 472 corpus-blocked scripts stay blocked in the pinned oracle; only live
  sessions resume `ActionWait`.
- Full behavioural effects for every one of the 772 actions, and full
  feat/Force-power/alignment combat depth.
- Audio/EAX, Bink movies, lip sync, animation/skinning, cutscenes, full `.gui`,
  minigames, localization, mods/Override, KotOR 2 world loading and P6 content.
- Persisting pending timers/closures across save/load. Saved script globals and
  object locals already round-trip; the live timer queue does not.

## Running the gate

```powershell
cd engine
cmd /c "scripts\build.bat msvc-release"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $ctest --preset msvc-release --output-on-failure
```

```powershell
.\build\msvc-release\kotor_engine.exe --module end_m01aa `
  --delayed-test "tests\fixtures\delay_test.ncs" --null --headless
```

The fixture source is `engine/tests/fixtures/delay_test.nss`; it is compiled
with the historical `nwnnsscomp` (`-v129`) and committed as `.ncs` so the test
does not depend on an external compiler at runtime.
