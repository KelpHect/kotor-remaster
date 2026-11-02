# Sprint 3 exit report — functional action runtime and P3/G3 residual closure

Status: **complete — gate S3 met and the P3/G3 residuals closed (2026-11-02).** Sprint 3 turned
the Sprint 2 mediated NCS VM into a stateful functional runtime for the shipped corpus and closed
the live differential, the 228 compiler-limited recompiles, and the 306 corpus-unused actions.

Baseline: Sprint 2 / gate S2 (`docs/SPRINT2_REPORT.md`): 772 action specs, engine map, NCS VM,
8,666/8,666 scripts at 0 faults with typed default stubs.

## Final source-level closure (2026-11-02)

The compiler work is complete. All **228/228** compiler-limited recompiles now come from
unchanged clean-room source with **no bytecode transform**:

- **24** historical prototype headers (default-argument arity);
- **201** struct recoveries: 189 duplicate trailing-return cleanup fixes, plus 12 files whose
  `ActionStartConversation` call is an 11-argument KotOR-era call against a 12-parameter header;
- **3** function-layout files compiled with the historical `nwnnsscomp` rebuild
  (`tools/compiler`), which discovers call arguments right-to-left.

`artifacts/lanes/s3-live/codegen_reconcile.json` reports `source_level 228`, `reconciled 0`,
`transforms {}`; every row is verified against the shipped NCS bytes. The full 8,666-script
rerun is **8,665 raw exact + 1 reachable-only** (`k_plev_space_hb`), with 0 compiler-limited
and 0 unexpected files.

## Exit criteria (gate S3)

| # | Criterion | Result | Evidence |
|---:|---|---|---|
| 1 | State model | **met** — `World`, `GameObject`, `Timer`, action-queue records; tags/areas, locals/globals, party, inventory, timers | `tools/vm/state.py`; `docs/recon/28_ACTION_MODEL.md`; 9 runtime tests |
| 2 | Functional action layer | **met** — 772/772 ids resolve; **772/772 runtime dispatch check**; 344 corpus actions covered by 44 specific + 300 generic handlers; **0 unclassified**; engine effects mediated | `tools/vm/actions_runtime.py`; `dispatch_check.py --runtime` (772 dispatched, 0 errors); `docs/recon/28_ACTION_MODEL.md`; functional corpus ledger |
| 3 | VM integration | **met** — runtime is optional; Sprint 2 typed-default mode still works; VM tests green | `tools/vm/ncs_vm.py`; 16 VM tests |
| 4 | Full-corpus run | **met — 8,666/8,666**; 8,185 returned, 472 blocked at `ActionWait`, 9 budget; 0 decode/VM faults, 0 unknown actions, 0 invalid targets | `tools/vm/corpus_run.py --runtime`; `docs/recon/29_FUNCTIONAL_RUNTIME.md`; `artifacts/lanes/s3-03-corpus/functional_corpus.json` |
| 5 | Reference-trace suite | **met** — 9 runtime tests cover globals, locals, tags/validity, inventory iterator, timers, action queue, `ActionWait`, strings, and a golden NCS set/get trace | `tools/vm/tests/test_action_runtime.py` |
| 6 | Tests/docs/repo | **met** — `run_all` 144 OK (1 skip), formats 211 OK, scripts 39 OK, VM 30 OK; install untouched; docs and report written | test logs; `git status` |
| 7 | Live differential | **met — 28/28 unique programs**; 400 `ExecuteCode` calls and 12,000 ACTION dispatches (hook limits); action id/IP/SP/BP and `sp_after` match with the engine's return cells injected. The `RunCommand` hook (`0x005d45d0`) captured **4,000** queued-command calls; its paired capture replays **30/30** programs exactly | `docs/recon/32_LIVE_DIFFERENTIAL.md`; `tools/vm/live_diff.py`; `artifacts/lanes/s3-live/live_diff7.json`; `live_diff_runcommand.json` |
| 8 | 228 compiler-limited recompiles | **met — 228/228 source-level, 0 ABI**: 24 historical headers, 201/201 structs (189 duplicate-return + 12 historical prototypes), 3 body-order via the historical compiler; full 8,666 rerun 8,665 raw exact + 1 reachable-only | `docs/recon/33_HISTORICAL_HEADERS.md`; `docs/recon/36_STRUCT_RECOVERY.md`; `docs/recon/37_RETURN_CLEANUP.md`; `docs/recon/38_BODY_ORDER.md`; `docs/recon/39_UNREACHABLE.md`; `docs/recon/35_FULL_RECOMPILE.md`; `docs/recon/31_COMPILER_CODEGEN.md` |
| 9 | 306 corpus-unused actions | **met — 306/306** invoked against a prepared world; 42 specific + 264 generic; **0 default**, 0 errors; **82 state-derived value assertions plus 20 SWMG set/get pairs**, with event/queue/mediation assertions for every action | `docs/recon/34_ACTION_BEHAVIOR.md`; `tools/vm/behavior_suite.py`; `artifacts/lanes/s3-02-actions/behavior_suite.json` |

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

`RunCommand` is also hooked: `FUN_005d45d0` (`0x005d45d0`) pushes the command frame and calls
`FUN_005d4270` (`0x005d4270`), which calls `ExecuteCode`. The run-command capture recorded
**4,000** queued-command calls (4,408 exits) and **400** paired `ExecuteCode` programs; its
30 unique programs replay **30/30** exactly.

The differential proved two VM fixes: `SAVEBP` points BP at the pushed saved-BP cell
(`len(stack)-1`), and KotOR string comparisons are case-insensitive (`"carth"` equals
`"Carth"`). Full method and anchors: `docs/recon/32_LIVE_DIFFERENTIAL.md`.

## 228 compiler-limited recompiles

The clean-room decompiler's source recovery is complete (8,428/8,666 raw byte-identical; 0
decompiler-side open). The 228 residuals split into three groups:

- **24 default-argument cases are fixed at source level.** `tools/scripts/historical_headers.py`
  recompiles the unchanged NSS with a per-file historical prototype header (the action is
  declared with the call site's original arity). Result: **24/24 raw byte-identical**; see
  `docs/recon/33_HISTORICAL_HEADERS.md`.
- **201 struct-group cases recovered at source level.** The shipped scripts use
  `struct tLastRound` (10 fields; `k_inc_generic.nss`). `tools/scripts/struct_source.py`
  restores the struct declaration at the exact global position and rewrites the flattened
  globals to field accesses; `tools/scripts/return_cleanup.py` then restores the compiler's
  duplicate trailing `return vN;`. Twelve of them also call `ActionStartConversation` with the
  KotOR-era 11 arguments while the header declares 12, so the per-file header is trimmed to the
  call-site arity. Result: **201/201 raw byte-identical**. See
  `docs/recon/36_STRUCT_RECOVERY.md`, `docs/recon/37_RETURN_CLEANUP.md`.
- **3 body-order cases** compile with the historical `nwnnsscomp` rebuild in `tools/compiler`,
  which discovers call arguments right-to-left and therefore assigns the same function
  addresses as the shipped compiler. Result: **3/3 raw byte-identical**; see
  `docs/recon/38_BODY_ORDER.md`.

The ledger is:

| class | files | meaning |
|---|---:|---|
| source headers | 24 | historical prototype arity, unchanged source |
| source struct | 201 | 189 duplicate-return + 12 historical prototypes |
| source body-order | 3 | historical compiler argument discovery |
| ABI | 0 | no bytecode transform remains |
| **total** | **228** | **228/228 source-level byte-identical** |

The full 8,666-script rerun is **8,665/8,666 raw byte-identical**, with **0 compiler-limited**
and **0 unexpected** files. The single remaining file is `k_plev_space_hb`: its discarded block
reuses one stack slot as an `int` in the loop condition and an `object` in the loop body, which
NWScript's type checker rejects (`docs/recon/39_UNREACHABLE.md`). The raw compiler output is
recorded and hashed per file. Evidence: `docs/recon/31_COMPILER_CODEGEN.md`,
`docs/recon/35_FULL_RECOMPILE.md`, `artifacts/lanes/s3-live/codegen_reconcile.json`.

## 306 corpus-unused actions

The shipped corpus uses 466 of the 772 action ids. `tools/vm/behavior_suite.py` prepares a world
with known object/world state and invokes all 306 unused actions:

```text
checked                 306/306
specific handlers          42
generic handlers          264
default/unclassified        0
state-derived assertions    82
SWMG set/get pairs       20/20
errors                      0
```

The 42 specific handlers cover math, string, journal, UI, effect-wrapper, and touch-attack
actions. The 264 generic handlers now resolve state where the action is a query (AC, saves,
alignment, trap/lock fields, faction, locations, journal, day/night, planets, last-event
records), maintain a live SWMG set/get state store (20/20 pairs), queue `Action*` records, and
record every mediated engine effect in the world event log. Evidence: `docs/recon/30_UNUSED_ACTIONS.md`, `docs/recon/34_ACTION_BEHAVIOR.md`,
`artifacts/lanes/s3-02-actions/behavior_suite.json`.

## What Sprint 3 does not claim

- Full combat resolution, dialogue resolution, renderer, audio, and module-transition behavior -
  P4/P5. Sprint 3 mediates and records these effects; it does not reproduce the engine systems.
- P6/P7 content and release work.
- The live run covers 28 unique programs (including 7 resumed/command bodies); the static
  full-corpus VM covers all 8,666 scripts.
- One shipped script (`k_plev_space_hb`) contains unreachable code that the decompiler dropped
  and that cannot be expressed in NWScript: the discarded block reuses one stack slot as an
  `int` and an `object`. It is the single `reachable_only` row; the other 8,665 scripts,
  including all 228 compiler-limited files, recompile byte-identically from source.

## Test totals

- `python tools/tests/run_all.py` → **144 tests, OK (1 pre-existing skip)**.
- `python -m unittest discover -s tools/formats -p "test_*.py"` → **211 tests, OK**;
  `validate_all.py` → `all_ok=True`.
- `python -m unittest discover -s tools/scripts/tests -p "test_*.py"` → **39 tests, OK**
  (includes the compiler-transform and historical-header samples).
- `python -m unittest discover -s tools/vm/tests -p "test_*.py"` → **30 tests, OK**
  (16 VM + 9 runtime/state + 2 unused-action + 2 live-diff + 1 behavior).

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
python tools/vm/live_diff.py \
  --events artifacts/lanes/s1-06-dynamic/runs/mesa-runcommand-20261102/events.jsonl \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --out artifacts/lanes/s3-live/live_diff_runcommand.json
python -m tools.scripts.historical_headers \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --nss artifacts/lanes/ncs-decompiler/cont2/out/nss \
  --header artifacts/lanes/ncs-decompiler/cont2/runs/incwhile-v10/bin/nwscript.nss \
  --compiler artifacts/lanes/04-ncs/tools/DeNCS/tools/nwnnsscomp_ktool.exe \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --corpus-json artifacts/lanes/s1-04-scripts/corpus.json \
  --taxonomy artifacts/lanes/ncs-decompiler/cont2/out/residual-taxonomy.json \
  --out-json artifacts/lanes/s3-live/historical_headers.json \
  --out-doc docs/recon/33_HISTORICAL_HEADERS.md
python -m tools.scripts.struct_source \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --nss artifacts/lanes/ncs-decompiler/cont2/out/nss \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --taxonomy artifacts/lanes/ncs-decompiler/cont2/out/residual-taxonomy.json \
  --compiler artifacts/lanes/04-ncs/tools/DeNCS/tools/nwnnsscomp_ktool.exe \
  --header artifacts/lanes/ncs-decompiler/cont2/runs/incwhile-v10/bin/nwscript.nss \
  --out-nss artifacts/lanes/s3-live/struct-nss \
  --out-json artifacts/lanes/s3-live/struct_source.json \
  --out-doc docs/recon/36_STRUCT_RECOVERY.md
python -m tools.scripts.return_cleanup \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --struct-nss artifacts/lanes/s3-live/struct-nss \
  --taxonomy artifacts/lanes/ncs-decompiler/cont2/out/residual-taxonomy.json \
  --compiler artifacts/lanes/04-ncs/tools/DeNCS/tools/nwnnsscomp_ktool.exe \
  --header artifacts/lanes/ncs-decompiler/cont2/runs/incwhile-v10/bin/nwscript.nss \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --out-nss artifacts/lanes/s3-live/struct-nss-final \
  --out-json artifacts/lanes/s3-live/return_cleanup.json \
  --out-doc docs/recon/37_RETURN_CLEANUP.md
python -m tools.scripts.unreachable \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --nss artifacts/lanes/ncs-decompiler/cont2/out/nss \
  --header artifacts/lanes/ncs-decompiler/cont2/runs/incwhile-v10/bin/nwscript.nss \
  --compiler artifacts/lanes/04-ncs/tools/DeNCS/tools/nwnnsscomp_ktool.exe \
  --taxonomy artifacts/lanes/ncs-decompiler/cont2/out/residual-taxonomy.json \
  --out-nss artifacts/lanes/s3-live/struct-nss-final \
  --out-json artifacts/lanes/s3-live/unreachable.json \
  --out-doc docs/recon/39_UNREACHABLE.md
# Build the historical compiler (tools/compiler/README.md), then:
python -m tools.scripts.body_order \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --nss artifacts/lanes/ncs-decompiler/cont2/out/nss \
  --header artifacts/lanes/ncs-decompiler/cont2/runs/incwhile-v10/bin/nwscript.nss \
  --compiler artifacts/lanes/s3-live/tools/nwnnsscomp_hist.exe \
  --taxonomy artifacts/lanes/ncs-decompiler/cont2/out/residual-taxonomy.json \
  --out-nss artifacts/lanes/s3-live/body-order-nss \
  --out-json artifacts/lanes/s3-live/body_order.json \
  --out-doc docs/recon/38_BODY_ORDER.md
python -m tools.scripts.codegen_reconcile \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --recompiled artifacts/lanes/s3-live/recon-input-v4 \
  --corpus-json artifacts/lanes/s1-04-scripts/corpus.json \
  --source-exact artifacts/lanes/s3-live/source_exact_all.json \
  --out-json artifacts/lanes/s3-live/codegen_reconcile.json \
  --out-doc docs/recon/31_COMPILER_CODEGEN.md
python tools/vm/behavior_suite.py \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --usage artifacts/lanes/s1-04-scripts/action_usage_report.json \
  --out-json artifacts/lanes/s3-02-actions/behavior_suite.json \
  --out-doc docs/recon/34_ACTION_BEHAVIOR.md
python -m tools.scripts.recompile_all \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --nss artifacts/lanes/ncs-decompiler/cont2/out/nss \
  --struct-nss artifacts/lanes/s3-live/struct-nss-final \
  --patched-compiler artifacts/lanes/s3-live/tools/nwnnsscomp_hist.exe \
  --header artifacts/lanes/ncs-decompiler/cont2/runs/incwhile-v10/bin/nwscript.nss \
  --compiler artifacts/lanes/04-ncs/tools/DeNCS/tools/nwnnsscomp_ktool.exe \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --corpus-json artifacts/lanes/s1-04-scripts/corpus.json \
  --taxonomy artifacts/lanes/ncs-decompiler/cont2/out/residual-taxonomy.json \
  --out-json artifacts/lanes/s3-live/recompile_all.json \
  --out-doc docs/recon/35_FULL_RECOMPILE.md
python tools/tests/run_all.py
```

## Public repository

New tracked files: `tools/vm/state.py`, `tools/vm/actions_runtime.py`,
`tools/vm/behaviors.py`, `tools/vm/behavior_suite.py`, `tools/vm/live_diff.py`,
`tools/vm/unused_actions.py`, `tools/vm/tests/test_action_runtime.py`,
`tools/vm/tests/test_action_behaviors.py`, `tools/vm/tests/test_unused_actions.py`,
`tools/scripts/codegen_reconcile.py`, `tools/scripts/historical_headers.py`,
`tools/scripts/struct_source.py`, `tools/scripts/return_cleanup.py`,
`tools/scripts/body_order.py`, `tools/scripts/unreachable.py`,
`tools/scripts/recompile_all.py`, `tools/scripts/tests/test_codegen_reconcile.py`,
`tools/compiler/nwnnsscomp-historical.patch`, `tools/compiler/README.md`,
`docs/recon/28_ACTION_MODEL.md`, `docs/recon/30_UNUSED_ACTIONS.md`,
`docs/recon/31_COMPILER_CODEGEN.md`, `docs/recon/32_LIVE_DIFFERENTIAL.md`,
`docs/recon/33_HISTORICAL_HEADERS.md`, `docs/recon/34_ACTION_BEHAVIOR.md`,
`docs/recon/35_FULL_RECOMPILE.md`, `docs/recon/36_STRUCT_RECOVERY.md`,
`docs/recon/37_RETURN_CLEANUP.md`, `docs/recon/38_BODY_ORDER.md`,
`docs/recon/39_UNREACHABLE.md`, this report.
No game assets, save contents, extracted payloads, or decompiled program text are tracked.
