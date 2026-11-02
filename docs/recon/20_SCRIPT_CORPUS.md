# Script corpus ledger — lane s1-04-scripts

Status: **public toolset promoted and full-corpus verification green.** All binary claims
below are against build SHA-256
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6` (verified read-only at
scan time, `install_check.matches_pinned_build: true`; chitin.key SHA-256
`2029ec1ee3f0e78a951320a7d9e3cfd0c21fc7ad6ec875e5de59a75f1c62df98`). Each claim is marked
**verified** (reproduced by the promoted tools in this lane), **inferred**, or **open**.

Lane spec note: `docs/SPRINT1.md` does not exist in the repo at this commit; the lane
scope was taken from the sprint-1 lane assignment (s1-04-scripts). **open** (doc gap, not a
data gap).

## Public toolset (new, tracked files — no game data)

| file | role |
|---|---|
| `tools/scripts/__init__.py` | package doc + pinned build hash + resource type constants |
| `tools/scripts/ncs.py` | NCS V1.0 codec promoted from `artifacts/lanes/04-ncs/cont/tools/ncs_encode.py`: `decode`/`encode` (readback + semantic modes), `roundtrip`, `iter_actions`, corpus CLI |
| `tools/scripts/actions.py` | 772-entry action-table loader (from `artifacts/action_table.json` produced by `tools/analysis/extract_actions.py`), arity rule, per-file/per-module usage, anomaly detection |
| `tools/scripts/status.py` | decompile-status classes (exact / compiler-limited / header-limited / reachable-only) + bug-parity flattening |
| `tools/scripts/corpus.py` | read-only install scan (KEY BIFs + `modules/*.rim` + `rims/*.rim` + `patch.erf`), sha256 dedupe, pairing ledger, per-module action usage, drift stop |
| `tools/scripts/tests/` | 29 synthetic-fixture tests (`python -m unittest discover -s tools/scripts/tests` → **29 run, 29 OK**) |

Provenance: the codec uses only the documented big-endian NCS V1.0 layout (13-byte header,
`0x42` size pseudo-op) — **verified** lossless below. The action id→name map is consumed
as data from `artifacts/action_table.json`, whose provenance is the shipped
`nwscript.nss` prototype order plus the `InitializeCommands` table writes at `0x0054c960`
/ tail `0x005cd2c0` and dispatcher `0x0052c0d0` in build `0290889e…74437e6`
(**verified** in `docs/recon/09_SCRIPTS.md` / `artifacts/action_table.json`:
772 slots, 774 writes, 772/772 assigned, ids 156/709 duplicated). No game assets,
extracted payloads, or decompiled program text are in `tools/scripts/`.

## Full-corpus verification (read-only, this lane's run)

Command: `python tools/scripts/corpus.py --game-dir <KOTOR_GAME_DIR> --table
artifacts/action_table.json --status artifacts/lanes/ncs-decompiler/cont2/out/status.jsonl
--out artifacts/lanes/s1-04-scripts --jobs 8` (2.5 s wall). Cross-tool:
`python tools/scripts/ncs.py --corpus artifacts/lanes/04-ncs/extracted/unique …` and
`python tools/scripts/actions.py --table artifacts/action_table.json --ncs-dir …`.

| metric | lane-04 published | this lane (verified) |
|---|---:|---:|
| NCS occurrences (BIFs + module RIMs + global RIMs + patch.erf) | 13,139 | **13,139** |
| unique NCS by SHA-256 | 8,666 | **8,666** |
| duplicate occurrences | 4,473 | **4,473** |
| decode failures | 0 | **0** |
| byte-identical re-encode, readback mode | 8,666/8,666 | **8,666/8,666** (`reencode_report.json`) |
| byte-identical re-encode, semantic mode | 8,666/8,666 | **8,666/8,666** |
| instructions | 4,703,036 | **4,703,036** |
| ACTION ops | 285,015 | **285,015** |
| unknown action ids (outside 0..771) | 0 | **0** (`unknown_action_ops: 0`) |
| distinct actions used | 466 (306 unused) | **466 (306 unused)** |
| arity-anomaly files | 5 | **5** (list below) |
| unique bytes | 28,670,606 | **28,670,606** |

Skeptic cross-check: sha→{resrefs, sources} from this lane's `corpus.json` vs lane-04
`corpus_index.jsonl`: identical key sets, **0/8,666 differing rows** (verified).

Artifacts: `artifacts/lanes/s1-04-scripts/corpus.json` (full ledger, 9.99 MB),
`module_action_usage.json` + `.csv`, `verification.json`, `reencode_report.json`,
`action_usage_report.json`.

## NSS ↔ NCS pairing ledger

For every unique NCS, `corpus.json` records all resref(s), all source archives, the
module identities (`modules/<mod>_s.rim` → `<mod>`), occurrence count, and the shipped
NSS entries with the same resref (resref + SHA-256 + source archive; payload bytes are
never written). Pairing is by resref string (case-insensitive), matching lane 04.

| metric | value (verified) |
|---|---:|
| NSS occurrences, all in `bif:data/scripts.bif` | 1,774 |
| NSS unique SHA-256 | 1,731 |
| base BIF NCS occurrences (`data/scripts.bif`) | 1,784 |
| unique NCS first seen in base BIF | 1,637 |
| unique NCS with a same-resref NSS anywhere | 1,680 |
| unique NCS without any NSS | 6,986 |
| **pairing coverage (unique NCS)** | **1,680 / 8,666 = 19.39%** |
| base-layer pairing coverage (unique base NCS with base NSS) | 1,611 / 1,637 = 98.41% |
| NSS resrefs with no matching NCS resref (source-only) | 20 (e.g. `k_inc_*`, `e3_scripts`, `fightback`) |

Note: the lane brief said "1,784 NSS in base"; measured is **1,774 NSS occurrences in
base** — 1,784 is the base **NCS** occurrence count (verified against
`corpus_summary.json` and this lane's scan). Pairing coverage is resref-based, so the
6,986 unpaired unique scripts are bytecode-only content (mostly module `_s` RIM
scripts); this matches lane-04 `corpus_pairing.json` (**verified**, same numbers).

## Decompile status per file

Merged from the clean-room decompiler run
`artifacts/lanes/ncs-decompiler/cont2/out/status.jsonl` (final-v10) by `input_sha256`;
8,666/8,666 corpus entries matched (**verified**). Classes per `docs/recon/09_SCRIPTS.md`:

| class | count | share | meaning |
|---|---:|---:|---|
| exact | 8,428 | 97.25% (recon/09 rounds 97.26%) | byte-identical NCS + full stream |
| compiler-limited | 228 | 2.63% | incl. 201 CPTOPBP+DESTRUCT slice, 24 default-arg materialization, 3 body-order (`k_pend_chest02`, `k_ren_visionland`, `k_sup_galaxmap`) |
| header-limited | 5 | 0.06% | recompile fails only vs the full header; byte-identical with lane-local prototype headers |
| reachable-only | 5 | 0.06% | differ only in unreachable code |

Decompiler-side open: **0** (verified from `residual-taxonomy.json`; carried from recon/09).

## Bug-parity items: the 5 action-arity anomalies

Detected by `tools/scripts/actions.py` as ACTION ops with argc outside
[required, declared] of the 772-entry table (arguments push right-to-left, so extras are
the deepest pushes and a missing slot is a stale read). Exactly the 5 known files
(**verified**, matches recon/09 and the cont2 `compile_fail` set):

| resref | sha256 (16) | action | argc seen | declared | kind | finding |
|---|---|---|---:|---:|---|---|
| `k_act_carthjoin` | `cdacb74da48c8d8b` | 574 `AddPartyMember` | 1 | 2 | missing | legacy 1-arg form; engine must accept it (content-critical join script) |
| `k_ptar_missjoin` | `9e042278e7aacbdc` | 574 `AddPartyMember` | 1 | 2 | missing | same legacy form (`OBJECT_SELF`) |
| `k_creditsplay` | `74148f25b0f3490e` | 518 `StartCreditSequence` | 2 | 1 | extra | extra trailing string inert and deepest |
| `k_plev_corpse1` | `a068b0c1242aef0c` | 700 `ActionBarkString` | 2 | 1 | extra | slot-2 semantics **open** (needs dispatcher evidence) |
| `nw_s0_lghtnbolt` | `966c757358a08d2e` | 469 `EffectBlasterDeflectionIncrease` | 2 | 1 | extra | slot-2 semantics **open** |

These 5 are also exactly the 5 header-limited decompile files — the recompiler cannot
express the anomalous call against the shipped prototypes, which is why they fail
there (verified: `status.jsonl` notes `[['main','action_missing_args',574,1,2], …]`).

## Per-module action usage

`artifacts/lanes/s1-04-scripts/module_action_usage.json` / `.csv`: for each of the
**117 module `_s` RIMs** (`modules/<mod>_s.rim`), the union of action ids from ACTION
ops of the NCS payloads in that RIM, occurrence-weighted within the RIM, with names from
the 772-entry table. Totals (verified): 117 module RIMs, 315,121 ACTION ops counted
over module-RIM occurrences (285,015 over the unique corpus; the difference is
duplicate occurrences across RIMs). Example: `end_m01aa_s` → module `end_m01aa`,
6,087 ACTION ops, 190 distinct actions (top: `Random` ×156, `SetFacing` ×18,
`PrintString` ×9).

## Open items

- `docs/SPRINT1.md` is now tracked (lane spec was taken from the assignment text on the first
  run); the document gap is **closed**.
- The two 700/469 extra-argument cases need engine dispatcher evidence (carried from recon/09) — **open**.
- The 306 unused actions still need engine implementations for modded content — **open** (shipped corpus does not exercise them; verified count).
- NSS-without-NCS (source-only) resrefs: 20; modded NSS without NCS is out of the shipped corpus — **open** by scope.
