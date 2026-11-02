# Sprint 3 exit report — functional action runtime and P3/G3 residual closure

Status: **complete — gate S3 met and the P3/G3 residuals closed (2026-11-02).** Sprint 3 turned
the Sprint 2 mediated NCS VM into a stateful functional runtime for the shipped corpus and closed
the live differential, the 228 compiler-limited recompiles, and the 306 corpus-unused actions.

Baseline: Sprint 2 / gate S2 (`docs/SPRINT2_REPORT.md`): 772 action specs, engine map, NCS VM,
8,666/8,666 scripts at 0 faults with typed default stubs.

## Exit criteria (gate S3)

| # | Criterion | Result | Evidence |
|---:|---|---|---|
| 1 | State model | **met** — `World`, `GameObject`, `Timer`, action-queue records; tags/areas, locals/globals, party, inventory, timers | `tools/vm/state.py`; `docs/recon/28_ACTION_MODEL.md`; 9 runtime tests |
| 2 | Functional action layer | **met** — 772/772 ids resolve; **772/772 runtime dispatch check**; 344 corpus actions covered by 44 specific + 300 generic handlers; **0 unclassified**; engine effects mediated | `tools/vm/actions_runtime.py`; `dispatch_check.py --runtime` (772 dispatched, 0 errors); `docs/recon/28_ACTION_MODEL.md`; functional corpus ledger |
| 3 | VM integration | **met** — runtime is optional; Sprint 2 typed-default mode still works; VM tests green | `tools/vm/ncs_vm.py`; 16 VM tests |
| 4 | Full-corpus run | **met — 8,666/8,666**; 8,185 returned, 472 blocked at `ActionWait`, 9 budget; 0 decode/VM faults, 0 unknown actions, 0 invalid targets | `tools/vm/corpus_run.py --runtime`; `docs/recon/29_FUNCTIONAL_RUNTIME.md`; `artifacts/lanes/s3-03-corpus/functional_corpus.json` |
| 5 | Reference-trace suite | **met** — 9 runtime tests cover globals, locals, tags/validity, inventory iterator, timers, action queue, `ActionWait`, strings, and a golden NCS set/get trace | `tools/vm/tests/test_action_runtime.py` |
| 6 | Tests/docs/repo | **met** — `run_all` 143 OK (1 skip), formats 211 OK, scripts 33 OK, VM 29 OK; install untouched; docs and report written | test logs; `git status` |
| 7 | Live differential | **met — 28/28 unique programs**; 400 `ExecuteCode` calls and 12,000 ACTION dispatches (hook limits); action id/IP/SP/BP and `sp_after` match with the engine's return cells injected; 21 fresh + 7 resumed/command bodies | `docs/recon/32_LIVE_DIFFERENTIAL.md`; `tools/vm/live_diff.py`; `artifacts/lanes/s3-live/live_diff7.json` |
| 8 | 228 compiler-limited recompiles | **met — 228/228 byte-identical** after the three named compiler-ABI transforms; raw ktool output hashed per file | `docs/recon/31_COMPILER_CODEGEN.md`; `tools/scripts/codegen_reconcile.py`; `artifacts/lanes/s3-live/codegen_reconcile.json` |
| 9 | 306 corpus-unused actions | **met — 306/306** invoked with typed args; 42 specific + 264 generic; **0 default/unclassified**, 0 errors | `docs/recon/30_UNUSED_ACTIONS.md`; `tools/vm/unused_actions.py`; `artifacts/lanes/s3-02-actions/unused_actions.json` |

## Functional corpus result

```text
count            8666
returned         8185
blocked           472   (ActionWait event loops)
budget              9   (polling loops)
decode errors       0
VM faults           0
unknown actions     0
invalid targets     0
ACTION calls   303,616
specific handlers   44
generic handlers   300
default actions      0
timers          20,406
queued actions   5,401
```

The 9 budget stops are loops that poll engine state without `ActionWait`; they are expected for
this headless runtime and are classified, not faults.

## Live `ExecuteCode` / action-dispatch differential

The live harness (`tools/harness/live_hooks.js`) records each `ExecuteCode` call (`0x005d2bd0`),
the full program bytes, entry IP/SP/BP and entry stack, and every action dispatch
(`0x0052c0d0`). `tools/vm/live_diff.py` seeds the VM with the engine's entry state, injects the
engine's return cells, and compares action id/IP/SP/BP and post-action stack depth.

Canonical run `mesa-vmdiff7-20261102`:

```text
ExecuteCode calls        400
ACTION dispatches     12,000
unique programs           28
matched exactly        28/28
fresh script bodies       21
resumed/command bodies     7
distinct actions          363
```

The differential proved two VM fixes: `SAVEBP` points BP at the pushed saved-BP cell
(`len(stack)-1`), and KotOR string comparisons are case-insensitive (`"carth"` equals
`"Carth"`). Full method and anchors: `docs/recon/32_LIVE_DIFFERENTIAL.md`.

## 228 compiler-limited recompiles

The clean-room decompiler's source recovery is complete (8,428/8,666 raw byte-identical; 0
decompiler-side open). The 228 residuals are compiler-ABI differences, reconciled by
`tools/scripts/codegen_reconcile.py`:

| transform | files | what it restores |
|---|---:|---|
| struct-group | 201 | the historical `CPTOPBP size=40; DESTRUCT` global-struct slice and the five-instruction compiler cleanup block |
| default-args | 24 | omission of trailing default arguments (restores `argc` and the original frame/jump operands) |
| body-order | 3 | function-body address order (historical emitted-JSR order; every body and callee identical) |

Result: **228/228 reconciled byte-identical** to the shipped NCS. The raw ktool output is
recorded and hashed per file; the source decompiler is not modified. Evidence:
`docs/recon/31_COMPILER_CODEGEN.md`, `artifacts/lanes/s3-live/codegen_reconcile.json`.

## 306 corpus-unused actions

The shipped corpus uses 466 of the 772 action ids. `tools/vm/unused_actions.py` invokes the other
306 with the prototype's typed default arguments and verifies the dispatch and return cell count:

```text
checked                 306/306
specific handlers          42
generic handlers          264
default/unclassified        0
errors                      0
```

The 42 specific handlers cover math, string, journal, UI, effect-wrapper, and touch-attack
actions added for this residual; the 264 generic handlers apply the documented semantic groups
(query, state/effect, action queue, UI, journal, audio, minigame). Evidence:
`docs/recon/30_UNUSED_ACTIONS.md`, `artifacts/lanes/s3-02-actions/unused_actions.json`.

## What Sprint 3 does not claim

- Full combat resolution, dialogue resolution, renderer, audio, and module-transition behavior —
  P4/P5. Sprint 3 mediates and records these effects; it does not reproduce the engine systems.
- P6/P7 content and release work.
- The live run covers 28 unique programs (including 7 resumed/command bodies); the static
  full-corpus VM covers all 8,666 scripts.

## Test totals

- `python tools/tests/run_all.py` → **143 tests, OK (1 pre-existing skip)**.
- `python -m unittest discover -s tools/formats -p "test_*.py"` → **211 tests, OK**;
  `validate_all.py` → `all_ok=True`.
- `python -m unittest discover -s tools/scripts/tests -p "test_*.py"` → **33 tests, OK**
  (includes the three compiler-transform samples).
- `python -m unittest discover -s tools/vm/tests -p "test_*.py"` → **29 tests, OK**
  (16 VM + 9 runtime/state + 2 unused-action + 2 live-diff).

## Reproduce

```bash
export KOTOR_GAME_DIR=/e/SteamLibrary/steamapps/common/swkotor
python tools/vm/dispatch_check.py \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json --runtime \
  --out artifacts/lanes/s3-02-actions/dispatch_check_runtime.json
python tools/vm/unused_actions.py \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --usage artifacts/lanes/s1-04-scripts/action_usage_report.json \
  --out-json artifacts/lanes/s3-02-actions/unused_actions.json \
  --out-doc docs/recon/30_UNUSED_ACTIONS.md
python tools/vm/corpus_run.py \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --budget 200000 --runtime \
  --out-json artifacts/lanes/s3-03-corpus/functional_corpus.json \
  --out-doc docs/recon/29_FUNCTIONAL_RUNTIME.md
python tools/vm/live_diff.py \
  --events artifacts/lanes/s1-06-dynamic/runs/mesa-vmdiff7-20261102/events.jsonl \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --out artifacts/lanes/s3-live/live_diff7.json
python -m tools.scripts.codegen_reconcile \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --recompiled artifacts/lanes/ncs-decompiler/cont2/out/ncs \
  --corpus-json artifacts/lanes/s1-04-scripts/corpus.json \
  --out-json artifacts/lanes/s3-live/codegen_reconcile.json \
  --out-doc docs/recon/31_COMPILER_CODEGEN.md
python tools/tests/run_all.py
```

## Public repository

New tracked files: `tools/vm/state.py`, `tools/vm/actions_runtime.py`,
`tools/vm/live_diff.py`, `tools/vm/unused_actions.py`, `tools/vm/tests/test_action_runtime.py`,
`tools/vm/tests/test_unused_actions.py`, `tools/scripts/codegen_reconcile.py`,
`tools/scripts/tests/test_codegen_reconcile.py`, `docs/recon/28_ACTION_MODEL.md`,
`docs/recon/30_UNUSED_ACTIONS.md`, `docs/recon/31_COMPILER_CODEGEN.md`,
`docs/recon/32_LIVE_DIFFERENTIAL.md`, this report.
No game assets, save contents, extracted payloads, or decompiled program text are tracked.
