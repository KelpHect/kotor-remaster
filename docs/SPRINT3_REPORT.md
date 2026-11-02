# Sprint 3 exit report — functional action runtime and object/timer model (P3 runtime gate)

Status: **complete — gate S3 met (2026-11-02).** Sprint 3 turned the Sprint 2 mediated NCS VM
into a stateful functional runtime for the shipped corpus, with deterministic object/tag/area,
local/global, inventory/party, action-queue, and timer semantics.

Baseline: Sprint 2 / gate S2 (`docs/SPRINT2_REPORT.md`): 772 action specs, engine map, NCS VM,
8,666/8,666 scripts at 0 faults with typed default stubs.

## Exit criteria (gate S3)

| # | Criterion | Result | Evidence |
|---:|---|---|---|
| 1 | State model | **met** — `World`, `GameObject`, `Timer`, action-queue records; tags/areas, locals/globals, party, inventory, timers | `tools/vm/state.py`; `docs/recon/28_ACTION_MODEL.md`; 9 runtime tests |
| 2 | Functional action layer | **met** — 772/772 ids resolve; **772/772 runtime dispatch check**; 344 corpus actions covered by 44 specific + 300 generic handlers; **0 unclassified**; engine effects mediated | `tools/vm/actions_runtime.py`; `dispatch_check.py --runtime` (772 dispatched, 0 errors); `docs/recon/28_ACTION_MODEL.md`; functional corpus ledger |
| 3 | VM integration | **met** — runtime is optional; Sprint 2 typed-default mode still works; VM tests unchanged and green | `tools/vm/ncs_vm.py`; 15 original VM tests |
| 4 | Full-corpus run | **met — 8,666/8,666**; 8,185 returned, 472 blocked at `ActionWait`, 9 budget; 0 decode/VM faults, 0 unknown actions, 0 invalid targets | `tools/vm/corpus_run.py --runtime`; `docs/recon/29_FUNCTIONAL_RUNTIME.md`; `artifacts/lanes/s3-03-corpus/functional_corpus.json` |
| 5 | Reference-trace suite | **met** — 9 runtime tests cover globals, locals, tags/validity, inventory iterator, timers, action queue, `ActionWait`, strings, and a golden NCS set/get trace | `tools/vm/tests/test_action_runtime.py` |
| 6 | Tests/docs/repo | **met** — `run_all` 138 OK (1 skip), formats 211 OK, scripts 32 OK; install untouched; docs and report written | test logs; `git status` |

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
ACTION calls   303,650
specific handlers   44
generic handlers   300
default actions      0
timers          20,406
queued actions   5,401
```

The 9 budget stops are loops that poll engine state without `ActionWait`; they are expected for
this headless runtime and are classified, not faults.

## What was built

- `tools/vm/state.py` — deterministic world/object model:
  invalid object `0x7F000000`, tags, area ids, numeric local indexes, typed globals, party,
  inventory, action queue, timers, RNG.
- `tools/vm/actions_runtime.py` — 44 specific action handlers plus generic semantic groups for
  `Get*/Is*/Has*`, `Set*/Clear*/Stop*/...`, `Action*` (queues), `Play*/Sound*/Music*/Speak*/...`
  (mediated), minigames (`SWMG_*`, `TUR_*`, `PAZ_*`), and the remaining engine-effect families.
- `tools/vm/ncs_vm.py` — optional runtime hook; `--runtime` CLI; runtime summaries in the result.
- `tools/vm/dispatch_check.py --runtime` — all 772 specs dispatch through the functional runtime
  (772/772, 0 errors).
- `tools/vm/corpus_run.py --runtime` — functional corpus ledger and coverage gate.
- `docs/recon/28_ACTION_MODEL.md` — state/action semantics and coverage.
- `docs/recon/29_FUNCTIONAL_RUNTIME.md` — generated full-corpus functional run.

## What Sprint 3 does not claim

- **Live `ExecuteCode`/`RunCommand` differential** against the original engine — P3/G3 follow-up.
- **Compiler-codegen parity**: the 228 compiler-limited byte-identical recompiles remain
  P3 compiler work.
- **The 306 unused actions** not exercised by the shipped corpus remain later P3 functional
  groups; all 772 ids still resolve to a handler.
- Full combat, dialogue resolution, renderer, audio, and module-transition behavior — P4/P5.

## Test totals

- `python tools/tests/run_all.py` → **138 tests, OK (1 pre-existing skip)**.
- `python -m unittest discover -s tools/formats -p "test_*.py"` → **211 tests, OK**;
  `validate_all.py` → `all_ok=True`.
- `python -m unittest discover -s tools/scripts/tests -p "test_*.py"` → **32 tests, OK**.
- `python -m unittest discover -s tools/vm/tests -p "test_*.py"` → **24 tests, OK**
  (15 VM + 9 runtime/state).

## Reproduce

```bash
export KOTOR_GAME_DIR=/e/SteamLibrary/steamapps/common/swkotor
python tools/vm/dispatch_check.py \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json --runtime \
  --out artifacts/lanes/s3-02-actions/dispatch_check_runtime.json
python tools/vm/corpus_run.py \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --budget 200000 --runtime \
  --out-json artifacts/lanes/s3-03-corpus/functional_corpus.json \
  --out-doc docs/recon/29_FUNCTIONAL_RUNTIME.md
python tools/tests/run_all.py
```

## Public repository

New tracked files: `tools/vm/state.py`, `tools/vm/actions_runtime.py`,
`tools/vm/tests/test_action_runtime.py`, `docs/recon/28_ACTION_MODEL.md`, this report.
No game assets, save contents, extracted payloads, or decompiled program text are tracked.
