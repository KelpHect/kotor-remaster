# P0 function classification

Status: **complete.** All 9,407 analyzed items (9,057 internal functions + 350 external imports) are
labeled as engine, CRT/runtime, middleware, thunk/import trampoline, compiler helper, or unknown,
with evidence and confidence. This closes the P0 classification item; it is a triage mask, not a
semantic specification of engine behavior.

## Result

Target: `swkotor.exe` SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
(KotOR 1.0.3.0, PE32 i386, image base `0x400000`). Analysis ran on a copy of the Lane 07 project;
the baseline `artifacts/ghidra/kotor.gpr` was never opened or modified.

| category | count | confidence |
|---|---:|---|
| engine | 8,527 | 5,052 high / 3,464 medium / 11 low |
| external import | 350 | 350 high |
| CRT / runtime | 417 | 414 high / 3 medium |
| thunk / import trampoline | 61 | 61 high |
| compiler helper | 30 | 30 high |
| statically linked middleware | 0 | — |
| unknown | 22 | 22 low |
| **total** | **9,407** | 5,907 high / 3,467 medium / 33 low |

The **engine-relevant denominator is 8,527**, not 9,057. Explicit engine roles: 565 action
handlers, the dispatcher `0x0052c0d0`, the action-table initializer `0x0054c960`, and 732
vtable/data-pointer methods. All 772 action IDs are covered by the 565 targets.

External imports by library: KERNEL32 113, OPENGL32 95, MSS32 61, USER32 40, BINKW32 20, GDI32 7,
IMM32 5, GLU32 3, VERSION 3, OLE32 2, DINPUT8 1. Middleware is DLL-hosted (Miles, Bink, OpenGL/GLU,
DirectInput), so it appears as external imports plus 14 import trampolines rather than internal code.

## Method

1. PE import directory parsed to 350 functions with IAT VAs, matched one-to-one to Ghidra external
   functions.
2. Thunks from Ghidra `isThunk`, `FF 25` jmp-[IAT], and single-`jmp rel32` veneers.
3. Action surface from the verified 565-target/772-ID map; dispatcher and initializer marked engine.
4. FID names split into 30 MSVC compiler/EH/security helpers and 417 CRT functions, corroborated by
   the contiguous static-CRT block `0x006fa390–0x0070c1e0` plus tail `0x00713190–0x00713e80`.
5. Engine region `0x00401000–0x006fa38f` (no CRT/helper FID names or block), corroborated by engine
   strings, OpenGL/GLU/MSS/Bink/DInput calls, pointer references, and call graph.
6. Tail C++ templates scored by engine callers/callees, `operator_new`, strings, and vtable pointers
   to separate them from the adjacent CRT tail.
7. A disconnected function with no callers/callees, strings, imports, or pointer references is left
   `unknown` rather than forced into engine by region alone.

Representative spot checks (decompiler text read, not name-only): `0x00406120` resref compare,
`0x00406d60` resref wrapper, `0x0052c0d0` dispatcher, `0x006fad9d` `fopen`, `0x006fc469` `fgets`,
`0x00705d37` security cookie, `0x0070c120` `gluBuild2DMipmaps` jmp-IAT thunk.

Validation: 16/16 invariants pass (9,407 rows; 9,057 internal; 350 external; unique addresses;
allowed categories; feature coverage; 565 action targets present and engine/handler; 772 IDs;
dispatcher/initializer engine; thunks classified; CRT block contains no engine body; every external
has an IAT; every unknown carries evidence).

## Residual unknowns

* **22 unknown** (0.24% of internal functions): disconnected small functions with no call-graph,
  string, import, or pointer evidence. They need data-xref/vftable recovery, runtime tracing, or
  manual decompilation.
* **3,464 medium-confidence engine rows** are region/call-graph decisions; raise confidence only
  with vftable membership, strings/asserts, or a spec.
* **440 action targets** are label-defined stubs; the classification covers dispatch identity, not
  handler semantics.
* The CRT boundary (`0x006fa390`, `0x0070c1e0`, `0x00713190`) is evidence-backed but has off-by-one
  risk; a skeptic sample is warranted.
* Middleware = 0 internal means no statically linked third-party library was found; the 32 functions
  that only call MSS32 and 5 Bink callers are engine audio/video wrappers holding engine state.

## Integration

Join on `address` + body hash + target SHA-256 when the evidence DB gains a function table. Use the
category mask to scope decompile/spec budgets and Gate G2 reporting; action rows feed the VM/action
registry. Full 9,407-row classification, evidence features, and scripts remain under
`artifacts/lanes/p0-classify/` (untracked). Digests: `classification.jsonl`
`809dc066…04eefd`, `counts.json` `1e13dd3a…05381f`, `validation.json` `51c6fa3e…19a1c7`.

Do not treat `engine` as "specified": specification coverage is still zero; this is triage.
