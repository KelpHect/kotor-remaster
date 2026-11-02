# Sprint 3 — P3 functional action runtime and object/timer model

Status: **complete — gate S3 met (2026-11-02)** — see
[`SPRINT3_REPORT.md`](SPRINT3_REPORT.md) for the exit report and evidence.

Goal: turn the Sprint 2 mediated NCS VM into a **stateful, functional script runtime** for the
shipped corpus. Sprint 3 covers the object/timer/action-queue model, explicit handlers for the
action groups used by the 8,666 unique scripts, and a synthetic reference-trace suite. The live
`ExecuteCode`/`RunCommand` differential against the original engine remains P3/G3 follow-up work;
compiler-codegen parity and the 306 unused actions remain later P3 work.

Baseline: Sprint 2 / gate S2 (`docs/SPRINT2_REPORT.md`): 772 action specs, engine map, NCS VM,
8,666/8,666 scripts at 0 faults with typed default stubs.

## Exit criteria (gate S3)

1. **State model** — `World`, `GameObject`, `Timer`, and action-queue records implement object
   identity/tags/areas, globals, locals, party, inventory, action queue, and delayed timers.
   Documented in `docs/recon/28_ACTION_MODEL.md`.
2. **Functional action layer** — all 772 action ids resolve to a runtime handler. The 345 ids
   exercised by the corpus have defined behavior: data getters/setters, object/tag/area queries,
   inventory/party, math/string/vector, action queue, and delayed timers. Engine-side effects
   (combat, dialogue, effects, rendering, audio) are mediated deterministically and logged.
   Zero unhandled action ids in the corpus.
3. **VM integration** — the runtime is optional: the Sprint 2 typed-default mode still works,
   and the functional mode passes the same VM/action bounds. No regressions in the VM tests.
4. **Full-corpus run** — 8,666/8,666 unique scripts execute with the functional runtime; zero
   decode errors, zero VM faults, zero unknown actions, zero invalid jump targets; a per-file
   ledger records status, actions, and state effects. The corpus action coverage table shows
   every used id resolved.
5. **Reference-trace suite** — synthetic NCS scripts with hand-authored expected traces cover
   globals, locals, tags, areas, inventory, party, action queue, delayed timers, and `ActionWait`;
   the VM trace matches the golden trace.
6. **Tests/docs/repo** — new tests green, `run_all` green, install untouched, no game assets or
   decompiled program text in tracked files, `docs/SPRINT3_REPORT.md` written with the S3 verdict.

## Lanes

| Lane | Scope | Owns (new files only) | Output root |
|---|---|---|---|
| s3-01-state | World/GameObject/timer/action-queue model | `tools/vm/state.py`, tests | `artifacts/lanes/s3-01-state/` |
| s3-02-actions | functional action registry and groups | `tools/vm/actions_runtime.py`, tests | `artifacts/lanes/s3-02-actions/` |
| s3-03-corpus | functional corpus run + action coverage | `tools/vm/corpus_run.py` updates | `artifacts/lanes/s3-03-corpus/` |
| s3-04-verification | golden traces, docs, report | `tools/vm/tests/`, `docs/recon/28_*.md` | `artifacts/lanes/s3-04-verification/` |

## Shared rules

- **Install:** read-only at `E:\SteamLibrary\steamapps\common\swkotor` (`KOTOR_GAME_DIR`);
  hash sentinels before/after. No original save or install write.
- **Evidence:** every claim needs a path, address, file hash, or test result.
- **Scope discipline:** the VM remains independent of the original executable; action effects are
  deterministic Python state, not engine calls.
- **Stop rule:** three identical failed attempts → document the exact failure and stop that item.

## Out of scope for Sprint 3

- Live `ExecuteCode`/`RunCommand` differential against the original engine — P3/G3 follow-up.
- Full combat, dialogue, renderer, audio, and module-transition behavior — P4/P5.
- The 228 compiler-limited byte-identical recompiles — P3 compiler-codegen work.
- The 306 unused actions not exercised by the shipped corpus — later P3 functional groups.
