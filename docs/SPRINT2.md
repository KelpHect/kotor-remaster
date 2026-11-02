# Sprint 2 — Binary engine map and NCS VM core (phase P2 closure, P3 foundation)

Status: **complete — gate S2 met (2026-11-02)** — see
[`SPRINT2_REPORT.md`](SPRINT2_REPORT.md) for the exit report and evidence.

Goal: close **P2 / gate G2** (the binary engine map and action specification surface) and
deliver the **P3 VM core** (an independent NCS interpreter with mediated actions), then run it
over **all 8,666 unique shipped NCS**. Functional action behavior and the full P3 differential
gate G3 remain later work; Sprint 2 proves the execution model, not the game systems.

Baseline: Sprint 1 / gate G1 complete (`docs/SPRINT1_REPORT.md`), P0 inventory v4
(`docs/recon/14_P0_FUNCTION_CLASSIFICATION.md`), script corpus (`docs/recon/20_SCRIPT_CORPUS.md`),
and the verified action table `artifacts/action_table.json` (772/772).

## Exit criteria (gate S2)

1. **Action spec stubs — 772/772.** Every action has a machine-readable spec with id, name,
   return type, parameter list, engine handler address, registration source, and a typed VM
   default stub. Zero missing names, return types, or handlers.
2. **Engine map — G2 closure.** The map records the 10,302 engine functions, the 772-slot action
   table, the dispatcher (`0x0052c0d0`, vtable `0x7450e4`), the initializer (`0x0054c960`), all
   565 unique handler targets, and the named subsystems/interfaces. Zero unknown or unexplained
   entry points (re-verified against the v4 inventory).
3. **NCS VM core.** An independent interpreter implements opcodes `0x01–0x2D`: typed 4-byte
   stack, SP/BP, `CPTOPSP`/`CPDOWNSP`, `CPTOPBP`/`CPDOWNBP`, `RSADD`, `CONST`, `ACTION`,
   arithmetic/logic/comparison, `JMP`/`JSR`/`JZ`/`JNZ`/`RETN`, `SAVEBP`/`RESTOREBP`,
   `STORESTATE`, `DESTRUCT`, `MOVSP`, and the increment/decrement ops. Recursion limit 8;
   bounded instruction budget for event-driven scripts.
4. **Action mediation.** All 772 action ids dispatch through the typed registry; unknown ids are
   hard errors; every action returns a typed default (int/float/string/object/vector/engine
   struct/void) without entering engine code. Action calls and argument counts are logged.
5. **Full-corpus execution.** All 8,666 unique NCS are decoded and executed from their entry
   point; zero unsupported opcodes, zero unknown actions, and zero VM faults. Results are
   classified (`returned`, `budget`, `blocked`) with per-file status, instruction count, stack
   depth, and action usage.
6. **Disassembly/control-flow parity.** For every unique NCS, the VM's decoded instruction count
   and jump targets match `tools/scripts/ncs.py`; synthetic opcode/stack tests pass.
7. **Evidence/repo.** New tests green, install untouched, no game assets or decompiled program
   text in tracked files, `docs/recon/26_ENGINE_MAP.md`,
   `docs/recon/27_VM_EXECUTION.md`, and `docs/SPRINT2_REPORT.md` written with the S2 verdict.

## Lanes

| Lane | Scope | Owns (new files only) | Output root |
|---|---|---|---|
| s2-01-engine-map | action specs, engine map, G2 evidence | `tools/analysis/action_specs.py`, `tools/analysis/engine_map.py`, `docs/recon/26_*.md` | `artifacts/lanes/s2-01-engine-map/` |
| s2-02-vm-core | NCS VM, typed values, action mediation, tests | `tools/vm/`, `docs/recon/27_*.md` | `artifacts/lanes/s2-02-vm-core/` |
| s2-03-vm-corpus | full 8,666-script execution ledger | `tools/vm/corpus_run.py` | `artifacts/lanes/s2-03-vm-corpus/` |
| s2-04-verification | cross-checks, docs, report, integration | tests and `docs/SPRINT2_REPORT.md` | `artifacts/lanes/s2-04-verification/` |

## Shared rules

- **Install:** read-only at `E:\SteamLibrary\steamapps\common\swkotor`
  (`KOTOR_GAME_DIR`); hash sentinels before/after. No original save or install write.
- **Hydralysis:** sealed `kotor` master, read-only; cite the build SHA-256
  `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6` with every address.
- **Data:** `artifacts/action_table.json` is the verified 772-entry action map;
  `artifacts/game-extract/nwscript.nss` is the read-only prototype source. Neither is committed.
- **Evidence:** every claim needs a path, address, file hash, or test result.
- **Stop rule:** three identical failed attempts → document the exact failure and stop that item.

## Out of scope for Sprint 2

- Functional implementations of the 772 actions (engine side effects, object model, timers,
  combat, dialogue) — P3 functional groups after the VM core.
- Live differential execution against the original engine (`ExecuteCode`/`RunCommand` hooks) —
  the Sprint 2 differential is static/opcode-level; the live harness is P3/G3.
- The 228 compiler-limited byte-identical recompiles and 306 unused actions — P3/G3.
- P4/P5 renderer, gameplay, and systems parity; P6/P7 content/release.
