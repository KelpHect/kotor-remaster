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

### VFS integration

`Localization::load(Install&, ...)` resolves the base `dialog.tlk` through the
VFS, then merges `Override/replace.tlk` (full replacement) and
`Override/append.tlk` (append), in that order. The integration fixture writes
both patches to a temporary overlay, loads them through `Install`, verifies
StrRef 0 is replaced and the appended StrRef resolves, and parses a synthetic
DLG whose `Text` StrRef points at the appended entry. Gate:
`tlk_vfs_replace`, `tlk_vfs_append`, `tlk_vfs_strref`.

## Faction parent inheritance

`FactionSystem` now tracks explicit matrix cells separately. A child with
`FactionParentID` and no explicit cell resolves through its parent (source
first, then target); `saveFac` writes only explicit cells so parents survive
round-trip. Synthetic 3-faction FAC gate covers parse, source/target fallback,
parent change and save/reload.

## CamVidEffect

The all-DLG corpus has **1,043 unique DLGs**, **22,572 entries**, **25,300
replies** and **67,395 links** (0 invalid), with **0** non-default
`CamVidEffect` values. Runtime dialogue increments
`unsupportedCamVidEffects_` and logs a warning instead of silently ignoring a
non-default value from a mod. A deterministic synthetic DLG with
`CamVidEffect=1` asserts the warning and rejection counter
(`camvid_fixture_effect=1`, `camvid_fixture_rejected=1`).

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
SithSoldier combat, `end_m01aa -> end_m01ab`, save/load) and an all-module
playthrough matrix: all **117 modules** load, settle for 120 frames and run
**every entity-owned conversation** end-to-end (**780 conversations**,
**10,133 dialogue lines**, 0 warnings). The retail GIT of `end_m01aa` references
`end_carth001`, which is absent from the VFS; retail cannot start it either, so
the gate asserts exactly this one known anomaly and zero engine failures. This
is the P6 behaviour gate; the VM corpus remains the action-level oracle (8,666
scripts / 303,616 actions).

## Bug parity

`--bug-parity-test` runs every documented glitch in classic and fixed modes:

- **Anywhere Menu Glitch** - classic opens the character panel during dialogue;
  fixed refuses.
- **Displaced Load Zone** - classic transitions through a door from an arbitrary
  position; fixed requires proximity (2 m).
- **Spawn Warps** - classic keeps party coordinates across a module transition;
  fixed uses the destination entry.
- **Hotshot** - classic restores the destination snapshot's level/XP while
  inventory and quests persist; fixed refuses.
- **Fake Level Up** - classic advances the level with XP unchanged; fixed
  refuses.
- **Item duping** - classic copies equipped items into the inventory on the
  airlock cycle; fixed does not.
- **The Routine** - the documented chain of menu glitch, hotshot, spawn warp,
  fake level and duping completes only in classic mode.

The models live in `engine/src/game/BugParity.h` and assert both the classic
mutation and the fixed-mode opt-out.

## Retail trace coverage and differential

`engine/scripts/check_retail_traces.py` parses the recorded s4 retail semantic
traces: **91,967 events**, **127 observed action IDs**, **772 implemented
action-table entries**, **0 missing**.

`kotor_retail_differential` records the engine's own semantic trace for the
new-game/Endar Spire path (`--retail-differential-test --engine-trace`) and
diffs it against `live-05-semantic.jsonl`:

- **7,756 engine actions**, **9 dialogue choices**, **1 transition**;
- **89** retail actions used by `end_m01aa` scripts;
- **367** executed action IDs across the playthrough, drained functional VM
  corpus (357) and eight probes;
- **0 missing execution**, **0 missing implementation** across all 102 retail
  action IDs.

The VM corpus drain executes delayed/queued closures (`--vm-drain`) so AI
actions behind `DelayCommand` are covered. Eight branch-gated actions are
recorded with phase `probe` (three talent/AI and five perception/movement
queries) because their combat or perception branches are state-gated.

## Evidence

- `docs/evidence/s14/differential.log`
- `docs/evidence/s14/save-matrix.log`
- `docs/evidence/s14/dialogue-corpus.log`
- `docs/evidence/s14/localization.log`
- `docs/evidence/s14/faction.log`
- `docs/evidence/s14/bug-parity.log`
- `docs/evidence/s14/retail-trace.log`
- `docs/evidence/s14/retail-trace-coverage.json`
- `docs/evidence/s14/engine-semantic.jsonl`
- `docs/evidence/s14/retail-differential.json`
- `docs/evidence/s14/retail-differential-engine.log`
- `docs/evidence/s14/vm-actions.log`
- `docs/evidence/s14/vm-actions.jsonl`
- `docs/SPRINT14_REPORT.md`
