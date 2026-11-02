# Sprint 2 exit report — binary engine map and NCS VM core (P2/G2, P3 foundation)

Status: **complete — gate S2 met (2026-11-02).** Sprint 2 closed P2 / gate G2 and built the
independent NCS VM core over the full shipped script corpus. Functional action behavior and the
full P3 differential gate G3 remain later work.

Baseline: Sprint 1 / gate G1 (`docs/SPRINT1_REPORT.md`), P0 inventory v4
(`docs/recon/14_P0_FUNCTION_CLASSIFICATION.md`), script corpus
(`docs/recon/20_SCRIPT_CORPUS.md`), and the verified 772-slot action table
(`artifacts/action_table.json`).

## Exit criteria (gate S2)

| # | Criterion | Result | Evidence |
|---:|---|---|---|
| 1 | 772 action specification stubs | **met — 772/772** | `tools/analysis/action_specs.py`; `artifacts/lanes/s2-01-engine-map/action_specs.json`; 772 return types, 772 handlers present, 565 unique handler targets |
| 2 | Engine map / G2 closure | **met** — 10,302 engine functions, 0 unknown, 0 unexplained `.text`, 772/772 actions, 565/565 handlers, dispatcher + initializer documented | `tools/analysis/engine_map.py`; `docs/recon/26_ENGINE_MAP.md`; `artifacts/lanes/s2-01-engine-map/engine_map.json` |
| 3 | NCS VM core | **met** — opcodes `0x01–0x2D`, typed 4-byte stack, SP/BP, `SAVEBP`/`RESTOREBP`, separate JSR/RETN return stack, `STORESTATE`, `DESTRUCT`, recursion limit 8 | `tools/vm/ncs_vm.py`; `tools/vm/tests/test_ncs_vm.py` **15/15** |
| 4 | Action mediation | **met — 772/772** registry entries; 345 action ids exercised; 0 unknown; functor args consume 0 cells; every non-void action returns a typed default | `action_specs.json`; corpus ledger `actions_used=345`, `unknown_action_ids=[]` |
| 5 | Full-corpus execution | **met — 8,666/8,666** decoded and executed; 0 decode errors, 0 VM faults, 0 unknown actions; 8,184 returned, 472 blocked at `ActionWait`, 10 budget stops | `tools/vm/corpus_run.py`; `docs/recon/27_VM_EXECUTION.md`; `artifacts/lanes/s2-03-vm-corpus/vm_corpus.json` |
| 6 | Disassembly/control-flow parity | **met** — every `JMP`/`JSR`/`JZ`/`JNZ` and `STORESTATE` target resolves to a decoded instruction; 0 invalid targets | `validate_control_flow()` in `tools/vm/ncs_vm.py`; corpus ledger |
| 7 | Evidence/repo/tests | **met** — 128 `run_all` tests OK (1 skip), 211 format tests OK, 32 script tests OK, 15 VM tests OK; install untouched; no assets in tracked files | test logs; `git status` |

## What was built

### P2 — action specification surface

`tools/analysis/action_specs.py` reads the verified action table, the shipped `nwscript.nss`
prototype source, and the v4 function export. It verifies the table and prototype order match
1:1, then emits all 772 records:

- id, name, return type, parameter list, required/total arity;
- engine handler name and address, with `handler_present` checked against the v4 inventory;
- typed default and stub text (`return 0`, `return ""`, `return OBJECT_INVALID`,
  `return vector(0.0, 0.0, 0.0)`, `return`).

Return-type mix: `void` 254, `int` 236, `object` 95, `effect` 87, `float` 48, `string` 26,
`vector` 11, `location` 6, `talent` 5, `event` 4. All 772 handler addresses are present in the
v4 export; the 772 actions map to 565 unique handler targets.

### P2 — engine map (gate G2)

`tools/analysis/engine_map.py` combines the v4 inventory with the action specs and writes
`docs/recon/26_ENGINE_MAP.md` + `engine_map.json`. The v4 inventory has 10,849 rows
(10,302 engine, 441 CRT, 76 thunks, 30 compiler helpers), 0 unknown, and the earlier
classification pass records 0 unexplained `.text` bytes. The map records the dispatcher
(`0x0052c0d0`, vtable `0x7450e4` slot `+8`, bound 772), the initializer (`0x0054c960`),
the 565 handler targets, and the known subsystem interfaces (script VM, resource/VFS,
save/GFF, renderer, input/camera, audio, chargen/UI, minimap).

### P3 foundation — independent NCS VM

`tools/vm/ncs_vm.py` executes NCS V1.0 from offset 0:

- typed 4-byte stack cells, `CPTOPSP`/`CPDOWNSP`/`MOVSP`, `CPTOPBP`/`CPDOWNBP`;
- `SAVEBP`/`RESTOREBP`, `RSADD`, `CONST`, `DESTRUCT`, `STORESTATE`;
- arithmetic, bitwise, comparison, `JMP`/`JSR`/`JZ`/`JNZ`/`RETN`, recursion limit 8;
- a separate JSR/RETN return stack, matching the documented NWScript calling convention;
- ACTION mediation with declared parameter widths (vector 3 cells, functor 0 cells, others 1)
  and typed defaults; no engine code is entered.

The shipped compiler's `MOVSP` convention was measured over the corpus: **all 699,919 shipped
`MOVSP` offsets are negative and pop cells** (`-4` 639,238; `-8` 20,824; `-12` 13,101; ...).
The first VM version had the sign reversed; correcting it removed all stack drift and the three
remaining heartbeat errors.

### Full-corpus execution

`tools/vm/corpus_run.py` runs all 8,666 unique NCS in 7.7 s:

```text
count=8666
status_counts: returned=8184, blocked=472, budget=10, error=0, decode_error=0
instructions: 3,895,590
action_calls: 306,116
actions_used: 345
unknown_action_ids: []
invalid_jump_targets: 0
max_sp: 217
gate: closed
```

`blocked` means the script reached `ActionWait` (id 202), the normal event-loop stop; `budget`
means a 200,000-instruction loop with no wait, which is expected for polled engine-state scripts.

## Test totals

- `python tools/tests/run_all.py` → **128 tests, OK (1 pre-existing skip: no save in the
  reference install itself)**; includes 15 VM tests and 17 mapcompile tests.
- `python -m unittest discover -s tools/formats -p "test_*.py"` → **211 tests, OK**;
  `validate_all.py` → `all_ok=True`.
- `python -m unittest discover -s tools/scripts/tests -p "test_*.py"` → **32 tests, OK**.
- `python -m unittest discover -s tools/vm/tests -p "test_*.py"` → **15 tests, OK**.

## What Sprint 2 does not claim

Sprint 2 proves the **execution model and dispatch surface**, not the game systems:

- The 772 action stubs return typed defaults; functional side effects (objects, timers,
  combat, dialogue, inventory, engine state) are still P3 functional-group work.
- The differential harness is static/opcode-level. Live `ExecuteCode`/`RunCommand` comparison
  against the original engine remains the P3/G3 differential task.
- The 228 compiler-limited byte-identical recompiles and 306 unused actions remain P3/G3
  residuals. The shipped corpus is fully decoded and now executes; recompilation parity is a
  compiler-codegen task, and the 306 actions are modded-content implementations.
- P4/P5 renderer, gameplay, and systems parity remain later phases.

## Reproduce

```bash
export KOTOR_GAME_DIR=/e/SteamLibrary/steamapps/common/swkotor
python tools/analysis/action_specs.py \
  --actions artifacts/action_table.json \
  --nwscript artifacts/game-extract/nwscript.nss \
  --functions artifacts/lanes/engine-semantics/export-v4/functions-v4.jsonl \
  --out artifacts/lanes/s2-01-engine-map/action_specs.json
python tools/analysis/engine_map.py \
  --functions artifacts/lanes/engine-semantics/export-v4/functions-v4.jsonl \
  --actions artifacts/action_table.json \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --out-json artifacts/lanes/s2-01-engine-map/engine_map.json \
  --out-doc docs/recon/26_ENGINE_MAP.md
python tools/vm/corpus_run.py \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --out-json artifacts/lanes/s2-03-vm-corpus/vm_corpus.json \
  --out-doc docs/recon/27_VM_EXECUTION.md
python tools/tests/run_all.py
```

## Public repository

New tracked tools: `tools/analysis/action_specs.py`, `tools/analysis/engine_map.py`,
`tools/vm/` (`ncs_vm.py`, `corpus_run.py`, tests), plus `docs/SPRINT2.md`, this report,
`docs/recon/26_ENGINE_MAP.md`, and `docs/recon/27_VM_EXECUTION.md`. No game assets, extracted
payloads, save contents, or decompiled program text are tracked. The install remains read-only.
