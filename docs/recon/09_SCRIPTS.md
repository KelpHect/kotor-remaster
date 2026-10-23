# NCS/NSS script corpus (lane 04)

Status: **corpus, validation, action usage, and a lossless bytecode codec complete.** Source-level
decompilation coverage is measured; the external decompiler is an oracle only. The original lane
hit its turn limit, and a continuation finished the codec, failure clustering, and arity analysis.
Extracted scripts and external tools are lane-local and untracked.

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

## Bytecode codec (verified lossless)

A clean-room decoder/encoder (`artifacts/lanes/04-ncs/cont/tools/ncs_encode.py`, independent of the
lane's reader) decodes every instruction and re-emits the stream big-endian behind the 13-byte
`NCS V1.0` header with a recomputed `0x42` size. Results over the unique corpus:

| metric | value |
|---|---:|
| unique scripts | 8,666 |
| byte-identical, readback mode | **8,666 / 8,666** |
| byte-identical, semantic mode | **8,666 / 8,666** |
| mismatches / mismatch offsets | 0 / none |
| instructions re-emitted | 4,703,036 |
| bytes re-emitted | 28,670,606 |

No divergence is semantic or size/padding-related. **NCS→IR→NCS is lossless for the shipped
corpus**, which gives a writer acceptance oracle without any decompiler. The codec is lane-local;
a public `tools/formats/ncs.py` waits on documenting the provenance of the 772-entry action table
(the codec itself uses only bytecode format facts).

## Source decompilation (measured, external oracle)

Each unique NCS was decompiled with **NCSDecomp 1.0.2** (DeNCS lineage; its bundle README is
**BSL 1.1**, not MIT) and recompiled with `nwnnsscomp_ktool.exe`; the result was compared with the
original bytecode:

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
207 files count as reproduced. An earlier raw pass reported 4,692 exact (54.14%); the final
classification supersedes it.

All 3,404 failures were re-decompiled and clustered. **99.53% are decompiler-side**, not parse or
bytecode failures:

| mechanism | files | share |
|---|---:|---:|
| decompiler stub (empty `main()`) | 1,262 | 37.07% |
| decompiler type inference | 1,011 | 29.70% |
| decompiler symbol table (`UnknownAction239`) | 607 | 17.83% |
| decompiler codegen | 508 | 14.92% |
| compiler produced no output | 10 | 0.29% |
| compiler stream mismatch | 6 | 0.18% |

The stub is a **scaling** failure: stub files are normal, large scripts (median 851 instructions);
failure rate rises from ~0–5% in the smallest deciles to **99.65%** in the largest. Highest area
rates are large-script areas (`tat_m18aa` 66.2%, `kas_m22aa` 66.1%, `korr_m39aa` 60.0%,
`end_m01aa` 55.2%). Since the bytecode codec is lossless, the path forward is NCS→IR→NSS recovery
validated by re-encoding, not the external compiler. NCSDecomp must not be shipped (BSL 1.1); it is
an offline oracle only.

## Action-arity anomalies (bytecode consistent; three explained, two open)

Arguments are pushed **right-to-left**, so `args[0]` is the last push. The `ACTION` instruction
carries the pop count; extras are the deepest pushes (they never shift declared parameters), and a
missing slot is an undefined/stale read, not zero.

| resref | action | argc | finding |
|---|---|---:|---|
| `k_act_carthjoin` | `AddPartyMember` (574) | 1 | legacy 1-arg `AddPartyMember(object)` form; engine must accept it |
| `k_ptar_missjoin` | `AddPartyMember` (574) | 1 | same legacy form (`OBJECT_SELF`) |
| `k_creditsplay` | `StartCreditSequence` (518) | 2 | extra trailing string is inert and deepest; declared int is unaffected |
| `k_plev_corpse1` | `ActionBarkString` (700) | 2 | slot 1 object, slot 2 int 1075; whether the engine reads slot 2 is **open** |
| `nw_s0_lghtnbolt` | `EffectBlasterDeflectionIncrease` (469) | 2 | slot 1 object, slot 2 0; slot-2 semantics **open** |

The two 574 sites are content-critical join scripts, so the K1 engine must implement the older
1-arg action form even though the shipped prototype declares two parameters. The 700/469 sites
need dispatcher evidence from a future Ghidra lane; they are recorded, not “fixed”.

## Open items

* Source-level decompilation is 60.72% reproducible (99.53% of failures are decompiler-side); the
  bytecode codec is lossless, so IR round-trip can replace compiler round-trip.
* The two 700/469 argument-position cases need the engine dispatcher.
* The 306 unused actions still need engine implementations for modded content and save/script
  compatibility, but shipped content does not exercise them.
* Public `tools/formats/ncs.py` waits on documenting the action table's provenance; the bytecode
  codec can be promoted after that review.
* NSS/NCS pairing by resref is complete, but source-only differences (modded NSS without NCS) are
  out of the shipped corpus.
