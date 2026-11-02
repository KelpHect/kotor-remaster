# Sprint 3 — P3 functional action runtime and object/timer model

Status: **complete — gate S3 met (2026-11-02)** — all 228 compiler-limited recompiles are
source-level byte-identical with zero ABI transforms. See
[`SPRINT3_REPORT.md`](SPRINT3_REPORT.md) for the exit report and evidence.

Goal: turn the Sprint 2 mediated NCS VM into a **stateful, functional script runtime** for the
shipped corpus, and close the P3/G3 residuals: the object/timer/action-queue model, explicit
handlers for the action groups used by the 8,666 unique scripts, a synthetic reference-trace
suite, the live `ExecuteCode`/action-dispatch differential against the original engine, the 228
compiler-limited byte-identical recompiles, and the 306 corpus-unused actions.

Baseline: Sprint 2 / gate S2 (`docs/SPRINT2_REPORT.md`): 772 action specs, engine map, NCS VM,
8,666/8,666 scripts at 0 faults with typed default stubs.

## Exit criteria (gate S3)

1. **State model** — `World`, `GameObject`, `Timer`, and action-queue records implement object
   identity/tags/areas, globals, locals, party, inventory, action queue, and delayed timers.
   Documented in `docs/recon/28_ACTION_MODEL.md`.
2. **Functional action layer** — all 772 action ids resolve to a runtime handler. The 344 ids
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
7. **Live differential** — replay every unique live `ExecuteCode` program in the VM and match the
   engine's action id/IP/SP/BP, post-action stack depth, and return cells exactly; capture the
   shared action-dispatch path and the queued-command runner (`0x005d45d0`). Evidence:
   `docs/recon/32_LIVE_DIFFERENTIAL.md`.
8. **228 compiler-limited recompiles** - all 228 are source-level byte-identical with zero
   bytecode transforms: 24 historical prototype headers, 201 struct recoveries (189
   duplicate-return + 12 historical `ActionStartConversation` prototypes), and 3 body-order
   files compiled with the historical `nwnnsscomp` rebuild. Full 8,666 rerun: 8,665 raw exact,
   1 documented reachability residual. Evidence: `docs/recon/33_HISTORICAL_HEADERS.md`,
   `docs/recon/36_STRUCT_RECOVERY.md`, `docs/recon/37_RETURN_CLEANUP.md`,
   `docs/recon/38_BODY_ORDER.md`, `docs/recon/39_UNREACHABLE.md`,
   `docs/recon/31_COMPILER_CODEGEN.md`, `artifacts/lanes/s3-live/codegen_reconcile.json`.
9. **306 corpus-unused actions** — every id outside the 466 used by the shipped corpus is invoked
   against a prepared world; 0 unclassified/default, 0 errors, and state-derived/mediation
   assertions for every action. Evidence: `docs/recon/34_ACTION_BEHAVIOR.md`,
   `artifacts/lanes/s3-02-actions/behavior_suite.json`.

## Lanes

| Lane | Scope | Owns (new files only) | Output root |
|---|---|---|---|
| s3-01-state | World/GameObject/timer/action-queue model | `tools/vm/state.py`, tests | `artifacts/lanes/s3-01-state/` |
| s3-02-actions | functional action registry and groups | `tools/vm/actions_runtime.py`, tests | `artifacts/lanes/s3-02-actions/` |
| s3-03-corpus | functional corpus run + action coverage | `tools/vm/corpus_run.py` updates | `artifacts/lanes/s3-03-corpus/` |
| s3-04-verification | golden traces, docs, report | `tools/vm/tests/`, `docs/recon/28_*.md` | `artifacts/lanes/s3-04-verification/` |
| s3-05-live | live `ExecuteCode`/dispatch differential | `tools/vm/live_diff.py`, hook trace probe | `artifacts/lanes/s3-live/` |
| s3-06-codegen | 228 compiler-codegen reconciliation | `tools/scripts/codegen_reconcile.py` | `artifacts/lanes/s3-live/codegen_reconcile.json` |
| s3-07-unused | 306 unused-action conformance | `tools/vm/unused_actions.py` | `artifacts/lanes/s3-02-actions/unused_actions.json` |

## Shared rules

- **Install:** read-only at `E:\SteamLibrary\steamapps\common\swkotor` (`KOTOR_GAME_DIR`);
  hash sentinels before/after. No original save or install write.
- **Evidence:** every claim needs a path, address, file hash, or test result.
- **Scope discipline:** the VM remains independent of the original executable; action effects are
  deterministic Python state, not engine calls.
- **Stop rule:** three identical failed attempts → document the exact failure and stop that item.

## Out of scope for Sprint 3

- Full combat resolution, dialogue resolution, renderer, audio, and module-transition behavior —
  P4/P5. Sprint 3 mediates and records these effects; it does not reproduce the engine systems.
- A new live scene with maximum queued-command coverage; the canonical run covers 28 unique
  programs, including 7 resumed/command bodies, and the dispatcher hook covers the shared path.
- P6/P7 content and release work.
