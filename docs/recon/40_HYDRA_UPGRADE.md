# 40 — Hydra Engine / Hydralysis upgrade and Sprint 0–3 impact check

Date: 2026-11-02. Status: **complete**. The KOTOR project now runs Hydralysis 0.8.0 on Hydra
Engine `12.3-hydra.6`. The upgrade invalidates no Sprint 0–3 evidence; the checks below show the
same function bounds, decompiles and write-set counts as the hydra.5 artifacts. Nothing from
Sprints 0–3 needs to be redone. The new `trace` and partial-decompile features are additive
follow-ups, not corrections.

## Versions

| | before | after |
|---|---|---|
| Hydralysis | 0.7.0 (`9205e9c`), 20 tools, catalog epoch 2 | **0.8.0** (`838c9bf`), **21 tools**, catalog epoch **5** |
| Hydra Engine | `12.3-hydra.5`, protocol 3 | **`12.3-hydra.6`**, protocol 3 |
| KOTOR backend | `tools/hydra-root/backends/hydra-12.3-hydra.5` | `.../hydra-12.3-hydra.6` |
| JDK | Adoptium 25.0.4.101-hotspot | unchanged |

The engine workspace `D:\Projects\Hydra` is `main` at `2ee5cb7` ("Bump submodules to the h6
releases"), with `hydra-engine` at `v12.3-hydra.6` (`5a1d8db393`) and `RusticRE`/Hydralysis at
`v0.8.0` (`838c9bf`). The project-local checkout `tools/hydralysis` is at the same `v0.8.0`.

## What changed

Hydralysis 0.8.0 / Hydra 12.3-hydra.6 add:

- **`trace`** (21st tool): bounded semantic-trace queries, explain/diff/check/export/import,
  evidence with `from_trace`, trace profiles and coverage gaps. Capture stays in `runtime`.
- **Decompile fallback and partial results**: a RusticQuery failure retries the engine's own
  `decompile` as a job; replies carry `partial`, `truncated`, `bytes_emitted`, `reason` and
  `fragments` (code/data/unknown), plus `--max-result-mb/kb` and `--offset` paging.
- MCP lanes/cancellation, warm publish workers, runtime memory authorization, and the engine
  trace snapshot-consistency fix (`Store::read_snapshot`).
- No change to the Ghidra upstream commit (`1b9e58a9...`), so static analysis is the same as
  hydra.5. The engine's Golden 9.1 KotOR gate passed 8,616/8,616 functions on both passes.

## Verification on this build

Target `kotor`: `swkotor.exe`, SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`,
image base `0x400000`.

```
backend inspect        ready true, hydra_release 12.3-hydra.6, pinned true, protocol 3, epoch 5
ghidra-query status    hydraVersion 12.3-hydra.6, target kotor, view master, revision 0
function va:0x006bb6f8 FUN_006bb670 body [006bb670,006bb73b]     (claim #18)
function va:0x004a0c00 FUN_004a0c00 body [004a0c00,004a0e23]     (claim #11)
writes offset 0xc4     57 sites; dialogue region 0x620000-0x640000: 0x630244, 0x63e695 only
                       (claim #19, same count and same two sites)
decompile 0x005d45d0   ok, 2,812 characters, truncated false      (Sprint 3 hook anchor)
decompile 0x00411730   ok, total 2,362 characters; caller cap 1,200 reports partial/truncated
call trace status      ok, 0 sessions; coverage "absence is not evidence of absence"
```

The three static checks above are the exact numbers written in `docs/recon/23_BINARY_P1.md`
(claims #11, #18, #19). The decompiles are full when not clipped by a caller cap; the new
partial flag describes the caller's cap, not an engine failure.

A second import of the same build is refused by design: `prepare --exe swkotor.exe --target
kotor-h6-check` returned `reused: true` for the existing `kotor` target (same SHA-256) and
created nothing. The adopted project is the stock Ghidra 12.1.4 project, opened read-only in
memory by h6. Fresh-analysis assurance therefore comes from the engine's own Golden 9.1 KotOR
gate (8,616/8,616 functions, `production` and `explicit`, on h6). The adopted project reports
8,966 analysed functions; the difference is the import/analysis profile, not an h6 regression,
and the address-level evidence above is unchanged.

## Sprint 0–3 impact

| Sprint | Hydra-dependent work | Impact of the upgrade | Redo? |
|---|---|---|---|
| 0 | Dataset/import/function-count claims (#10), adopted target registration | Same PE, same Ghidra upstream commit; `backend inspect` ready; target hash matches. | **No.** |
| 1 | Binary P1 evidence #11–#32, decompiles, write scans | Function bounds, 57-site `+0xc4` write set and both dialogue sites match; `FUN_005d45d0` decompiles fully. New fallback/partial only affects failed or clipped decompiles; none of the recorded S1 decompiles failed. | **No.** Optional: re-run any large-function decompile with `--max-result-mb` now that partial results are explicit. |
| 2 | 772-action engine map, handler labels, dispatcher map | Addresses and analysis are unchanged; action specs were not derived from decompiler text. | **No.** Optional: a trace capture could observe real dispatch, but Sprint 3 already covers this. |
| 3 | 8,666/8,666 NCS recompiles, 306 unused actions, live ExecuteCode/RunCommand differential | The NCS pipeline is independent of Hydra (clean-room Python decompiler + nwnnsscomp). The live differential's 28/28 and 30/30 results are unaffected. | **No.** Optional: engine-native `trace` evidence could cross-check the same programs, but it does not replace the existing exact replay. |

## Recommended follow-ups (additive, not a redo)

1. **Author a KOTOR trace profile** for the conversation/camera and RunCommand paths. The
   repository ships no KOTOR profile, and `trace start` requires a build-bound profile. With one,
   the `coverage=unknown` limitations on the camera claims can become measured coverage, and
   the Sprint 3 live differential gains an engine-native evidence bundle.
2. **Use partial decompile deliberately** for any function that previously hit a size cap: pass
   `--max-result-mb` and inspect `fragments`/`reason` instead of treating the reply as failed.
3. **Do not commit the adopted project** with h6 unless a one-way upgrade is intended. The
   KOTOR target is still the stock `artifacts/ghidra/kotor.gpr` opened read-only; no Hydralysis
   lineage or generation exists, and no backup is needed for this read-only use.

## Operational notes

- The Hydralysis MCP catalog epoch changed (2 → 5) and the old server held
  `tools/.venv/Scripts/hydralysis.exe` open during the upgrade. The two KOTOR MCP process trees
  were stopped to replace the executable; **restart the OpenCode session/harness** to load the
  21-tool catalog. The CLI works in the meantime.
- `backend doctor`: 0 errors, 2 NTFS copy-on-write warnings (root and state), unchanged.
  `backend status` reports `daemon: not_running` until a `rusticre` daemon is started; the CLI
  `ghidra-query` starts the engine on demand, as these checks did.
- The separate Cyberpunk 2077 Hydralysis install (`D:\Projects\CoopCyberpunk`) and its running
  jobs were not touched.
- The installed Vibe skills were refreshed from the v0.8.0 checkout: `hydralysis` (now says
  twenty-one tools) and the new `trace` skill.

## 2026-11-03 recheck: function-inventory reconciliation

The upgrade exposed one difference worth recording, not a Sprint failure: the stock `kotor`
target and the P0 v4 inventory describe different function sets.

| target | internal functions | external | total | relation |
|---|---:|---:|---:|---|
| `kotor` (stock adopted project, h6) | 8,616 | 350 | 8,966 | strict subset of v4 |
| `kotor-v4` (P0 v4 project, h6, read-only) | 10,849 | 350 | 11,199 | exact P0 inventory |

`kotor-v4` was adopted read-only on 2026-11-03 from
`artifacts/lanes/engine-semantics/export-v4/project` (Ghidra 12.1.4, `-noanalysis -readOnly`).
It lists **10,849/10,849** `functions-v4.jsonl` entries and **565/565** action handlers; the
stock target lists 8,616, all of which are in the v4 set (no h6-only function). The 2,233 v4
entries absent from the stock project are pointer-created entry points (1,005 vtable methods,
440 action handlers, 15 thunks, 24 CRT, the rest engine code), not missing bytes: sample sites
`0x00401a30` and `0x0052c6a0` disassemble as ordinary functions.

Evidence claims that name such an entry point cannot be queried on the stock target. Claim #2
(dispatcher `0x0052c0d0`) and claim #16 (supermodel query `0x00484d20`) were re-run on
`kotor-v4`: `NWScript_DispatchAction` body `[0052c0d0,0052c0fa]`, and `FUN_00484d20` body
`[00484d20,00484d39],[00484d40,00484df1]`, decompiling normally. The v4 target is therefore the
reproduction target for P0/S2 inventory claims; the stock `kotor` target remains the h6 Golden
target (8,616 functions, matching the engine's KotOR Golden gate).

Other rechecks:

- `writes` counts match the old claims on h6: `+0xc4` 57 sites (claim #19), `+0x38` 265 sites
  with the same four camera-region sites (claim #18), `+0x1d0` 48 sites (claim #22).
- P0 export: 10,849 rows, 0 empty C, 0 `decompile_error`, 0 truncation; 4 rows used the
  300-second timeout; no row is clipped at a 32,000-character cap (max 171,244).
- Every Sprint 1–3 gate JSON inspected has its booleans true: 8,666/8,666 recompiles, 228/228
  source-level, 3/3 body-order, 5/5 unreachable, 306/306 actions, 8,666/8,666 functional
  corpus, 772/772 specs and dispatch.
- No KOTOR trace profile exists in the repository, so `trace` is installed but not applicable
  without authoring one. It is an additive follow-up; the existing live camera observation and
  the 28/28 + 30/30 live differential already cover the dynamic gates.

Verdict: **Sprints 0–3 remain 100% against their declared gates; no redo.** The only change is
that P0/S2 inventory claims should cite `kotor-v4` when the address is one of the v4 pointer
entry points.
