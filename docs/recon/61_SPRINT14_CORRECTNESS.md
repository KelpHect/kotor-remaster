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

`kotor_retail_differential` records engine semantic traces for the new-game
path, the Taris/Taris Lower City/Unknown World saves, four module-warp
destinations, and every other module entry path. The 108 additional module
traces came from one retail native-loader RPC session (`s14-allmodules-01`):
116 targets were queued after the initial Taris save, and the trace was split
at each `currentgame\<module>.rim` transition. The four warp traces predate the
sweep and are retained.

- **120 differential paths**, all with module identity checks;
- **117 of 117 modules** covered (`retail-path-coverage.json` complete);
- **178 unique retail action IDs** across the corpus;
- engine traces for every module, including the 7,767-action Endar Spire
  playthrough and per-module `--module-trace` runs;
- **376-378** executed action IDs depending on the path (**377** on the Endar
  Spire playthroughs and most module traces) across the playthroughs, drained
  functional VM corpus (357) and nineteen
  probes;
- **0 missing execution**, **0 missing implementation** for every path, with a
  module-identity check that rejects a trace whose recorded module differs from
  the engine module.

The VM corpus drain executes delayed/queued closures (`--vm-drain`) so AI
actions behind `DelayCommand` are covered. Nineteen branch-gated actions are
recorded with phase `probe` (talent/AI, perception/movement, combat-result and
persistent-object queries) because their branches are state-gated. The four
save traces were recorded by replacing the profile QUICKSAVE slot; the module
sweep uses the native `StartNewModule` thunk through a Frida RPC and is split
with `tools/harness/split_live_modules.py`.

## Evidence

- `docs/evidence/s14/differential.log`
- `docs/evidence/s14/save-matrix.log`
- `docs/evidence/s14/dialogue-corpus.log`
- `docs/evidence/s14/localization.log`
- `docs/evidence/s14/faction.log`
- `docs/evidence/s14/bug-parity.log`
- `docs/evidence/s14/retail-trace.log`
- `docs/evidence/s14/retail-trace-coverage.json`
- `docs/evidence/s14/engine-semantic-end_m01aa.jsonl`
- `docs/evidence/s14/engine-semantic-tar_m02af.jsonl`
- `docs/evidence/s14/engine-semantic-tar_m03aa.jsonl`
- `docs/evidence/s14/engine-semantic-unk_m41ad.jsonl`
- `docs/evidence/s14/engine-semantic-unk_m41ab.jsonl`
- `docs/evidence/s14/live-taris-semantic.jsonl`
- `docs/evidence/s14/live-tarislc-semantic.jsonl`
- `docs/evidence/s14/live-unkautosave-semantic.jsonl`
- `docs/evidence/s14/live-unkgame1-semantic.jsonl`
- `docs/evidence/s14/retail-differential.json`
- `docs/evidence/s14/retail-trace-sources.json`
- `docs/evidence/s14/retail-path-coverage.json`
- `docs/evidence/s14/console-warp-attempts.log`
- `docs/evidence/s14/retail-differential-s4-live-02.json`
- `docs/evidence/s14/retail-differential-s4-live-03.json`
- `docs/evidence/s14/retail-differential-s4-live-04.json`
- `docs/evidence/s14/retail-differential-s4-live-05.json`
- `docs/evidence/s14/retail-differential-taris-save.json`
- `docs/evidence/s14/retail-differential-tarislc-save.json`
- `docs/evidence/s14/retail-differential-unkautosave-save.json`
- `docs/evidence/s14/retail-differential-unkgame1-save.json`
- `docs/evidence/s14/retail-differential-engine-end_m01aa.log`
- `docs/evidence/s14/retail-differential-engine-tar_m02af.log`
- `docs/evidence/s14/retail-differential-engine-tar_m03aa.log`
- `docs/evidence/s14/retail-differential-engine-unk_m41ad.log`
- `docs/evidence/s14/retail-differential-engine-unk_m41ab.log`
- `docs/evidence/s14/vm-actions.log`
- `docs/evidence/s14/vm-actions.jsonl`
- `engine/tests/fixtures/retail_traces.json`
- `tools/harness/semantic_from_events.py`
- `docs/SPRINT14_REPORT.md`
