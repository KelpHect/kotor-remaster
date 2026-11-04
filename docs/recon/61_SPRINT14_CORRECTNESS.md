# Recon 61 - Sprint 14 correctness evidence

Status: Sprint 14 implementation note. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## TLK merge

`Tlk::merge(base, patch, name, replaceCount)` implements the TSLPatcher shape:

- first `replaceCount` patch entries overwrite base StrRefs;
- empty text keeps the base text, empty sound keeps the base sound;
- remaining patch entries append after the base;
- the base `Tlk` is not mutated.

Fixture: overwrite StrRef 0, sound-only patch StrRef 1 while keeping its text,
append one entry at `base.size()`. Gate in `docs/evidence/s14/localization.log`.

## Faction parent inheritance

`FactionSystem` now tracks explicit matrix cells separately. A child with
`FactionParentID` and no explicit cell resolves through its parent (source
first, then target); `saveFac` writes only explicit cells so parents survive
round-trip. Synthetic 3-faction FAC gate covers parse, source/target fallback,
parent change and save/reload.

## CamVidEffect

The all-DLG corpus has **1,043 unique DLGs**, **22,572 entries** and **25,300
replies** and **67,395 links** (0 invalid), with **0** non-default
`CamVidEffect` values. Runtime dialogue now
increments `unsupportedCamVidEffects_` and logs a warning instead of silently
ignoring a non-default value from a mod.

## Save/load matrix

Seven retail fixtures:

| Slot | Module | Objects matched |
|---|---|---:|
| 000000 QUICKSAVE | tar_m02af | 13 |
| 000001 AUTOSAVE | unk_m41ad | 56 |
| 000002 Game1 | unk_m41ab | 63 |
| 000003 SYNTH | m12ab | 0 (synthetic tags) |
| 000004 VANILLA | END_M01AA | 111 |
| 000005 TARIS | tar_m02af | 13 |
| 000006 TARISLC | tar_m03aa | 79 |

For each slot the gate loads the save, checks module/gold/inventory/journal/
party/companions/position and saved object state (position, dead, HP, door
open/locked, placeable loot), writes a new save, verifies the flattened GIT has
no `UseTemplates`, then reloads and re-checks. Duplicate tags are resolved by
nearest base position, and `GameSession::loadOriginalSave` uses the same
matching so all saved states are applied.

The shipped install has no external `<module>.rsv`; the gate generates a
temporary module-ERF `.rsv` from the vanilla save and asserts that its
`Mod_Entry_X` shadows the nested save module. The same cached module IFO then
carries a malformed `EventQueue` list and still loads with the `.rsv` sentinel
position, proving the cached-queue hazard is non-fatal.

## Differential gate

`--differential-test` runs the save matrix, the entire DLG corpus, the
deterministic Endar Spire playthrough (Trask dialogue, trigger, locker,
SithSoldier combat, `end_m01aa -> end_m01ab`, save/load) and an eight-module
playthrough matrix (`end_m01aa`, `end_m01ab`, `tar_m02af`, `tar_m03aa`,
`tar_m03ab`, `danm14aa`, `manm28aa`, `m12ab`) with 7 dialogue starts and 143
dialogue lines. This is the P6 behaviour gate; the VM corpus remains the
action-level oracle (8,666 scripts / 303,616 actions).

## Evidence

- `docs/evidence/s14/differential.log`
- `docs/evidence/s14/save-matrix.log`
- `docs/evidence/s14/dialogue-corpus.log`
- `docs/evidence/s14/localization.log`
- `docs/evidence/s14/faction.log`
- `docs/SPRINT14_REPORT.md`
