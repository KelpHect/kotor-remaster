# NCS/NSS script corpus (lane 04)

Status: **corpus, validation, and action usage complete; decompiler coverage measured.** The lane
hit its turn limit before writing a curated final report, so this document summarizes the finished
datasets. Extracted scripts and the external tools are lane-local and untracked.

## Corpus

Read-only scan of the owned install (BIFs and module RIMs):

| item | occurrences | unique SHA-256 | bytes (unique) |
|---|---:|---:|---:|
| NCS (type 2010) | 13,139 | 8,666 | 28,670,606 |
| NSS (type 2009) | 1,774 | 1,731 | — |

4,473 NCS occurrences are duplicates by content. Every unique NCS was extracted and hashed; the
base layer is `data/scripts.bif` (1,784), the rest come from module `_s.rim` archives. NSS source is
shipped for only 1,731 unique scripts; the remainder are bytecode-only.

## Structural validation (verified)

All 8,666 unique NCS payloads decode with a big-endian reader (`NCS V1.0`, 13-byte header, `0x42`
size pseudo-op):

* 8,666/8,666 valid, 0 failures.
* 4,703,036 instructions; 37 of the 45 known opcodes occur in shipped code.
* 285,015 `ACTION` instructions; **0 unknown action IDs** against the 772-entry table.
* 5 files with operand/length warnings; 5 files with an action-arity anomaly (below).
* 6,061 files contain unreachable instructions after `RETN`/`JMP` (compiler output, not corruption).
* 466 of 772 actions are used; 306 actions are never referenced by shipped scripts.

Top actions by occurrence: `GetIsObjectValid` 20,675, `GetHasSpell` 19,203,
`GetBaseItemType` 11,839, `TalentSpell` 10,566, `GetIsTalentValid` 9,198,
`GetObjectByTag` 8,749, `IntToString` 7,558, `GetCreatureTalentRandom` 7,226,
`GetCreatureHasTalent` 6,965, `AssignCommand` 6,455, `Random` 6,446, `ClearAllActions` 5,685,
`GetIdFromTalent` 5,580, `DelayCommand` 5,494, `GetFirstPC` 5,397,
`GetPartyMemberByIndex` 5,185, `ActionPlayAnimation` 4,967, `GetRacialType` 4,903,
`GetHasSpellEffect` 4,450, `GetWaypointByTag` 3,882. Argument counts range 0–12
(most common: 1 argument, 145,093; 2, 82,806; 0, 18,109).

Data: `corpus_summary.json`, `ncs_validation_summary.json`, `action_usage.json`,
`decompile_status.csv/jsonl` (per unique script).

## Decompile round-trip (measured, external oracle)

Each unique NCS was decompiled with **NCSDecomp 1.0.2** (DeNCS lineage, MIT) and recompiled with
`nwnnsscomp_ktool.exe`; the result was compared with the original bytecode:

| outcome | files | share |
|---|---:|---:|
| byte-identical | 5,055 | 58.33% |
| normalized (default-argument expansion) | 207 | 2.39% |
| **reproducible** | **5,262** | **60.72%** |
| compile error | 3,383 | 39.04% |
| compile produced no output | 15 | 0.17% |
| instruction-stream mismatch | 6 | 0.07% |
| **failed** | **3,404** | **39.28%** |

“Normalized” means the recompiled instruction stream matches after expanding default arguments; the
207 files are counted as reproduced. An earlier raw pass reported 4,692 exact (54.14%); the final
classification supersedes it and separates normalized matches from failures.

Failure causes are decompiler/compiler limitations, not parse failures: DeNCS emits incomplete
source (`No code could be decompiled`, stub branches) or source that `nwnnsscomp` rejects. A
different decompiler or a direct NCS→IR→NCS path is required for the remaining 3,404 scripts. The
public corpus does not currently include a reusable NCS decompiler; the lane tools are external
oracles only.

## Action-arity anomalies (verified, cause open)

Five shipped scripts encode an argument count that disagrees with the nwscript signature:

| resref | action | encoded argc | signature argc |
|---|---|---:|---:|
| `k_act_carthjoin` | `AddPartyMember` | 1 | 2 |
| `k_ptar_missjoin` | `AddPartyMember` | 1 | 2 |
| `k_creditsplay` | `StartCreditSequence` | 2 | 1 |
| `k_plev_corpse1` | `ActionBarkString` | 2 | 1 |
| `nw_s0_lghtnbolt` | `EffectBlasterDeflectionIncrease` | 2 | 1 |

These are either shipped-script bugs, compiler-version differences, or stack-effect conventions
that need one runtime oracle. They are recorded, not “fixed”; the engine must reproduce the
original behavior for these five.

## Open items

* Decompiler coverage is 60.72% reproducible; no public tool proves all 8,666 scripts.
* The 306 unused actions still need engine implementations for modded content and save/script
  compatibility, but shipped content does not exercise them.
* Arity anomaly semantics (pop count vs ignored argument) are unverified.
* NSS/NCS pairing by resref is complete, but source-only differences (modded NSS without NCS) are
  out of the shipped corpus.
