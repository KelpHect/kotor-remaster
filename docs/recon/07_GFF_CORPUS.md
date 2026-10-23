# GFF corpus validation (lane 03)

Status: **complete**. Every GFF-magic payload in the shipped 1.0.3.0 install parses and every field
value resolves under the corrected structural invariants. The public parser was patched from this
lane's proposal; the corpus was re-validated with that patched parser.

## Scope and method

Read-only scan of the owned install:

* `data/*.bif` through `chitin.key` — 26 BIFs, 25,836 indexed resources
* `modules/*.rim` — 234 RIMs
* `rims/*.rim` — 12 RIMs
* `patch.erf` — 97 entries
* `TexturePacks/*.erf` — 4 ERFs
* `lips/*.mod` — 123 MOD V1.0 ERF-family archives, checked separately: 0 GFF payloads
* `Override/` is empty; no save archives exist in this install

Detection is payload-based (known 4CC or printable 4CC + `V3.2`), not declared-type-based. The
extracted payloads live under the untracked lane directory and are not committed.

## Totals

| source | GFF resources | parsed | intrinsic failures |
|---|---:|---:|---:|
| BIF | 1,362 | 1,362 | 0 |
| module RIM | 9,712 | 9,712 | 0 |
| global RIM | 62 | 62 | 0 |
| patch ERF | 7 | 7 | 0 |
| **total** | **11,143** | **11,143** | **0** |

* 47,178,217 payload bytes, 10,270 unique payload hashes, all GFF `V3.2`.
* Declared resource type and payload magic agree on 11,143/11,143 resources.
* 236,699 structs, 1,854,982 fields, 128,539 lists, 215,078 list entries, 443,842 label entries.
* 1,854,982/1,854,982 field values resolved; 0 parse failures, 0 bounds/value failures.
* Field-data totals 5,727,013 bytes with 0 unreferenced and 0 overlapping bytes.
* List-index totals 1,376,536 bytes; no overlapping ranges; 2,068 unreferenced bytes in 15 files.

Magics and counts: `ARE 117, BIC 1, BTC 3, BTI 1, DLG 1167, FAC 20, GIT 117, GUI 91, IFO 117,
ITP 16, JRL 4, PTH 132, UTC 1958, UTD 575, UTE 133, UTI 1055, UTM 38, UTP 1784, UTS 635, UTT 1121,
UTW 2058`. `BTC`/`BTI` (declared types 2026/2024) are valid GFF V3.2 containers.

## Corrected structural invariants

The shipped corpus disproved one invariant in the original `gff.check()`: multi-field struct
field-index ranges are **not** packed in struct-index order. Files order ranges by creation (the root
is often written last) and may carry stale unreferenced entries. Validating them as packed produced
4,740 false-positive messages on 55 files (GIT 11, UTM 16, FAC 16, ITP 9, DLG 2, BIC 1).

The corrected rule validates only that each multi-field struct's slot range is in bounds and that
ranges are disjoint. After the fix: 11,143/11,143 parse, check, and value checks pass; 0 failures.

Other shipped quirks are warnings, not corruption:

* 5,464 duplicate `SoundExists` labels in 29 template DLGs. `as_dict()` keeps one value; the new
  `as_pairs()` preserves duplicates.
* 890 stale field-index entries in 30 files (unreferenced copies).
* 2,068 unreferenced list-index bytes in 15 GIT/ITP files.

## Type coverage 0–18

Observed in shipped data: 0 uint8, 2 uint16, 3 int16, 4 uint32, 5 int32, 8 float, 10 string,
11 resref, 12 locstring, 13 binary, 14 struct, 15 list, 16 vector4, 17 vector3.

Not observed in shipped data: **1 int8, 6 uint64, 7 int64, 9 double, 18 strref**. They are covered
by generated fixtures only, so engine behaviour on them is corpus-untested. Type 18 is not used
standalone; all 87,705 shipped locstrings carry a StrRef.

Confirmed wire details:

* Type 13 binary uses a `u32` length; 117 fields, 1,872 bytes, exact field-data coverage.
* CExoLocString writes `total = 8 + substring_bytes` (it excludes its own 4 bytes); 87,705/87,705
  match. The parser now validates the total, substring count, and substring lengths.
* CExoString and ResRef lengths are byte counts; bounds are validated instead of truncating.

## Parser changes promoted

`tools/formats/gff.py` now has:

1. Corrected `check()` slot rule (disjoint, in-bounds; no packing requirement).
2. Explicit field-data bounds for types 6/7/9/10/11/12/13/16/17 with value/offset in errors.
3. Struct→field→label table contiguity and 4-byte field/list-index length checks.
4. CExoLocString count/length/`total` validation.
5. Clean `list_of()` bounds errors.
6. `as_pairs()` for duplicate record labels.

Regression tests: `tools/formats/test_gff_types.py`, 30 tests, all passing. The patched parser was
re-run over the corpus: 11,143/11,143 parse, check, and value checks pass.

## Open items

* Types 1/6/7/9/18 have no shipped instances; inline vs offset variants of 6/7/9 are fixture-only.
* Modded GFFs can violate vendor conventions (for example a wrong locstring `total`); the strict
  checks are intended for the shipped corpus and evidence tooling, not as a mod loader contract.
