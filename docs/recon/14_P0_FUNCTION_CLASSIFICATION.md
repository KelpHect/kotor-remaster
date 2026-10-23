# P0 function classification

Status: **v3 complete for the analyzed inventory: 9,407/9,407 high confidence, 0 medium, 0 low,
0 unknown, 48/48 checks.** A separate completeness pass is adding three engine functions that the
original Ghidra inventory missed (`0x699c10`, `0x45e180`, `0x474220`) and resolving four decompiler
refusals. Classification is a triage mask, not a semantic specification of engine behavior.

## Result

Target: `swkotor.exe` SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
(KotOR 1.0.3.0, PE32 i386, image base `0x400000`). Analysis ran on throwaway copies; the baseline
`artifacts/ghidra/kotor.gpr` was never opened or modified.

| category | count | confidence |
|---|---:|---|
| engine | 8,549 | 8,549 high |
| external import | 350 | 350 high |
| CRT / runtime | 417 | 417 high |
| thunk / import trampoline | 61 | 61 high |
| compiler helper | 30 | 30 high |
| statically linked middleware | 0 | — |
| unknown | 0 | — |
| **total** | **9,407** | **9,407 high** |

The engine-relevant denominator is **8,549**. Explicit roles: 565 action handlers, the dispatcher
`0x0052c0d0`, the action-table initializer `0x0054c960`, and 738 vtable/data-pointer methods. All
772 action IDs are covered by the 565 targets.

External imports by library: KERNEL32 113, OPENGL32 95, MSS32 61, USER32 40, BINKW32 20, GDI32 7,
IMM32 5, GLU32 3, VERSION 3, OLE32 2, DINPUT8 1. Middleware is DLL-hosted, so it appears as external
imports plus 14 import trampolines rather than internal code.

## Method

1. PE import directory → 350 externals with IAT VAs, matched one-to-one to Ghidra.
2. Thunks from `isThunk`, `FF 25` jmp-[IAT], and single-`jmp rel32` veneers.
3. Action surface from the verified 565-target/772-ID map.
4. FID names split into 30 compiler/EH/security helpers and 417 CRT functions, corroborated by the
   static-CRT block `0x006fa390–0x0070c1e0` plus tail `0x00713190–0x00713e80`.
5. Engine region `0x00401000–0x006fa38f` plus call-graph/string/import/pointer scoring.
6. Tail C++ templates separated from the CRT tail by callers/callees, `operator_new`, strings, and
   vtable pointers.
7. **v2:** decompiled all 9,057 internal functions, recovered label-only callers, scanned 363
   vtable/pointer tables, and probed RTTI (absent, `/GR-`). Confidence requires one direct signal or
   two independent evidence families.
8. **v3:** resolved the 76 v2 medium rows with a second evidence family: 54 caller-context chains
   anchored in engine strings/globals/vtables/actions (every hop a direct caller), 11 address-taken
   function-pointer installs, 8 ctor/dtor arguments, 3 unwind-funclet targets, 7 engine globals,
   61 body-semantic observations, and 11 object-field/vtable-slot call sites. No row was left
   irreducible; 0 medium / 0 unknown.

Representative spot checks (decompiler text read, not name-only): `0x00406120` resref compare,
`0x00406d60` resref wrapper, `0x0052c0d0` dispatcher, `0x006fad9d` `fopen`, `0x006fc469` `fgets`,
`0x00705d37` security cookie, `0x0070c120` `gluBuild2DMipmaps` jmp-IAT thunk.

Validation: **48/48** checks (29 v2 invariants + 19 v3 token/site/count checks).

## Bearing code proof

The same Ghidra pass proved the coordinate sign from code: door loader `0x50a0e0` and placeable
loader `0x50a7b0` read `Bearing` B, build `q=(cos B/2,0,0,sin B/2)` about +Z (`0x4ac960`), compute
`forward=(−sin B, cos B, 0)` (`0x5853c0`/`0x5896e0`), and store it at `+0x9c` (`0x4cd170`). The save
path inverts with `atan2(−fx, fy)` (`0x4aa0f0`, `0x586a70`); creatures carry
`X/Y/ZOrientation` directly (`0x4c8c70`). See `bearing_code_proof.md` and `docs/recon/12`.

## Residuals

* **Three engine functions outside the original inventory** (`0x699c10` object helper,
  `0x45e180` DDS texture loader, `0x474220` geometry code). They add no rows to the 9,407 set but
  must be added to the inventory; a completeness pass is scanning `.text` for any other missed
  functions.
* **Four decompile refusals** are documented; classification uses graph/string/thunk evidence.
  Disassembly-level resolution is part of the completeness pass.
* **440 action targets** are label-defined stubs; classification covers dispatch identity, not
  handler semantics (P2/P3 work).
* The CRT boundary is evidence-backed but retains off-by-one risk at
  `0x006fa390`/`0x0070c1e0`/`0x00713190`; a skeptic sample is warranted.
* Middleware = 0 internal; the 32 MSS32-only and 5 Bink callers are engine audio/video wrappers.

## Integration

Join on `address` + body hash + target SHA-256. Use the category mask for decompile/spec budgets and
Gate G2; action rows feed the VM/action registry. Full classification, evidence, scripts, and the
Bearing proof remain under `artifacts/lanes/p0-classify/cont{,2}/` (untracked).

Do not treat `engine` as "specified": specification coverage is still zero; this is triage.
