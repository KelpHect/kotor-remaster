# P0 function inventory and classification (v4)

Status: **exhaustive and all high-confidence:** 11,199 rows (10,849 internal functions + 350 external
imports), 0 medium/low/unknown, 34/34 strict checks, and **0 unexplained `.text` bytes**. The four
decompiler refusals were 60-second timeouts and now decompile at 300 seconds. A consolidation pass
to export all v4 pseudocode and update the ledger is queued; the classification itself is complete.

## Result

Target: `swkotor.exe` SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
(KotOR 1.0.3.0, PE32 i386, image base `0x400000`). All work used throwaway copies; the baseline
`artifacts/ghidra/kotor.gpr` was never opened or modified.

| category | count | confidence |
|---|---:|---|
| engine | 10,302 | all high |
| external import | 350 | all high |
| CRT / runtime | 441 | all high |
| thunk / import trampoline | 76 | all high |
| compiler helper | 30 | all high |
| unknown | 0 | — |
| **total** | **11,199** | **11,199 high** |

v4 adds 1,792 internal functions over v3 (9,407 rows): +1,753 engine, +24 CRT, +15 jump thunks,
and 1,005 new vtable methods. The engine-relevant denominator is **10,302**. Explicit roles include
565 action handlers, dispatcher `0x0052c0d0`, initializer `0x0054c960`, and vtable/pointer methods.
All 772 action IDs remain covered.

## `.text` coverage

Virtual `.text` = 3,387,856 B (`0x401000`–`0x73C1D0`):

| category | bytes | share |
|---|---:|---:|
| function instructions | 3,101,994 | 91.6% |
| EH funclets | 127,686 | 3.8% |
| padding | 96,436 | — |
| data tables | 22,311 | — |
| other code fragments | 17,306 | — |
| unowned orphan/continuation instructions | 22,123 | 0.7% |
| **unexplained** | **0** | — |

The 4,861 orphan instructions have no call or pointer target except one (`0x709d66`, created); they
are continuation blocks after jump tables, not function entries. Every function entry is an
instruction start that passes a register-liveness test and decodes to a terminator.

## Discovery method

* v2: all 9,057 internal functions decompiled; label-only callers recovered; 363 vtable/pointer tables
  scanned; RTTI absent (`/GR-`).
* v3: 76 medium rows resolved with second evidence families (caller chains anchored in engine
  strings/globals/vtables/actions, address-taken installs, unwind funclets, body semantics).
* v4: full analysis of a throwaway copy added 159 entries; byte-level evidence added 1,641 more from
  aligned pointer cells/vtable runs, direct jump targets, and canonical-prologue blocks. 11
  mid-function fragments were deleted; 1,794 additions were audited by decompilation. The three
  previously missing engine functions (`0x45e180`, `0x474220`, `0x699c10`) are now rows with
  decompiled evidence.
* Four decompile refusals (`0x44f070`, `0x5972b0`, `0x5eeb10`, `0x656d20`) were timeouts; at 300 s
  they emit 23–171 KB of C.

## Bearing code proof

Door loader `0x50a0e0` and placeable loader `0x50a7b0` read `Bearing` B, build
`q=(cos B/2,0,0,sin B/2)` about +Z (`0x4ac960`), compute `forward=(−sin B, cos B, 0)`
(`0x5853c0`/`0x5896e0`), and store it at `+0x9c` (`0x4cd170`). Save inverts with
`atan2(−fx,fy)` (`0x4aa0f0`, `0x586a70`); creatures carry `X/Y/ZOrientation` directly (`0x4c8c70`).
See `bearing_code_proof.md`.

## Residuals

* **Pseudocode export/ledger consolidation** for all 10,849 internal functions is queued; the
  classification rows and audit decompilations exist lane-locally.
* **22,123 unowned continuation bytes** are accounted but not function-owned; they are after jump
  tables/terminators, with one created entry.
* **440 action targets** are label-defined stubs; classification covers dispatch identity, not
  handler semantics (P2/P3).
* CRT boundary off-by-one risk at `0x006fa390`/`0x0070c1e0`/`0x00713190`; a skeptic sample is
  warranted.
* Middleware = 0 internal; MSS32/Bink callers are engine wrappers.

## Evidence

`artifacts/lanes/p0-classify/cont3/` (untracked): `classification.jsonl`, `counts.json`, `diff.json`,
`validation.json`, `summary.md`, `out/coverage_v7.json`, `out/coverage_accounting.json`,
`out/refusal_diag*`, `out/audit_new.jsonl`, `scripts/`. Bearing proof:
`cont2/bearing_code_proof.md`.

Do not treat `engine` as "specified": specification coverage is still zero; this is triage.
