# Sprint 14 report - P6 correctness and residual gaps

Status: **implementation complete; gate S14 met for the recorded retail corpus
(8 paths, 5 of 117 modules)**. Full per-quest retail coverage is an outstanding
data-capture item: 112 modules have no recorded retail trace. Sprint 13 closed
the runtime/media/GUI/localization/mods/K2-world/faction/P6-content systems.
Sprint 14 closed the correctness gaps and added the save-matrix, eight-path
retail differential and all-seven bug-parity evidence.

## 1. Build and test result

```text
100% tests passed, 0 tests failed out of 51
Total Test Time (real) = 847.96 sec
```

New CTest targets: `kotor_save_matrix`, `kotor_differential`,
`kotor_dialogue_corpus`, `kotor_bug_parity`, `kotor_retail_trace` and
`kotor_retail_differential`. The existing 45 targets still pass.

## 2. Differential playthrough gate

`--differential-test` combines:

- **Retail save differential** - seven retail save fixtures (quicksave,
  autosave, Game1, synthetic, vanilla, Taris, Taris Lower City) load, match
  their saved gold/inventory/journal/party/position and object state, then
  save and reload through the engine.
- **Dialogue/cutscene corpus** - **1,043 unique shipped DLGs**, **22,572
  entries**, **25,300 replies**, **5,900 scripts**, **6,715 sounds** and
  **22 camera models**, **67,395 dialogue links**, all parsed with zero failures
  and zero invalid links.
- **Deterministic Endar Spire playthrough** - Trask dialogue (9 lines), trigger
  `end_trig02`, locker use, SithSoldier combat (faction-correct), module
  transition `end_m01aa -> end_m01ab`, and save/load of `end_m01ab`.
- **All-module playthrough matrix** - every one of the **117 modules** loads,
  settles for 120 frames, and runs **every entity-owned conversation** through
  the dialogue state machine: **780 conversations**, **10,133 dialogue lines**,
  **0 warnings**. One retail GIT references a DLG absent from the VFS
  (`end_carth001`); retail cannot start it either, so it is asserted as a known
  data anomaly rather than an engine failure.

Evidence: `docs/evidence/s14/differential.log`.

## 3. Save/load matrix

`--save-matrix-test` covers **7/7** fixtures:

- loads each slot and matches module, gold, inventory, journal, party,
  companions and position;
- applies saved object states (position, dead, HP, door open/locked, placeable
  loot) with duplicate-tag handling;
- writes a new save, verifies the nested GIT has **no `UseTemplates` field**
  (flattened state), then reloads and re-checks gold/inventory/journal/party;
- **external `<module>.rsv` shadowing** - a synthetic module ERF `.rsv` with a
  `Mod_Entry_X` sentinel shadows the nested save module (`rsv_shadow=1`);
- **corrupt `EventQueue`** - the same cached module IFO carries a malformed
  `EventQueue` list and still loads with the sentinel position
  (`eventqueue_shadow=1`).

Evidence: `docs/evidence/s14/save-matrix.log`.

## 4. TSLPatcher TLK merge and VFS integration

`Tlk::merge(base, patch, name, replaceCount)` supports:

- overwrite of the first `replaceCount` entries;
- empty text/sound fields keeping the base value;
- append of the remaining patch entries;
- base TLK unchanged.

`Localization::load(Install&, ...)` now resolves the base `dialog.tlk` through
the VFS and merges `Override/replace.tlk` (full StrRef replacement) followed by
`Override/append.tlk` (append). A DLG `StrRef` then resolves through the merged
table.

Gate: `tlk_merge_size`, `tlk_merge_overwrite`, `tlk_merge_sound_only`,
`tlk_merge_append`, `tlk_merge_base_unchanged`, `tlk_vfs_replace_write`,
`tlk_vfs_append_write`, `tlk_vfs_load`, `tlk_vfs_replace`, `tlk_vfs_append`,
`tlk_vfs_strref`.
Evidence: `docs/evidence/s14/localization.log`.

## 5. Faction parent inheritance

`FactionSystem` now tracks explicit cells separately from inherited values.
A child faction with `FactionParentID` falls back to its parent row (source and
target), and `saveFac` writes only explicit cells while preserving parents.
Gate: `fac_parent_parse`, `fac_parent_id`, `fac_parent_source`,
`fac_parent_target`, `fac_parent_friendly`, `fac_parent_set`,
`fac_parent_set_value`, `fac_parent_save`, `fac_parent_roundtrip`.
Evidence: `docs/evidence/s14/faction.log`.

## 6. CamVidEffect audit and fixture

The dialogue corpus has **0** non-default `CamVidEffect` values across all
1,043 DLGs. Runtime dialogue records and warns on a non-default value instead
of silently ignoring it. A deterministic synthetic DLG with `CamVidEffect=1`
asserts the warning path and the rejection counter.
Gate: `dialogue corpus: camvid_defaults=1`, `camvid_fixture_effect=1`,
`camvid_fixture_rejected=1`.
Evidence: `docs/evidence/s14/dialogue-corpus.log`.

## 7. Bug-parity mode

`--bug-parity-test` runs every documented glitch in classic and fixed modes:

- **Anywhere Menu Glitch** - classic mode opens the character panel during
  dialogue; fixed mode refuses it.
- **Displaced Load Zone** - classic mode transitions through a door from an
  arbitrary position; fixed mode requires proximity.
- **Spawn Warps** - classic mode keeps party coordinates across a module
  transition; fixed mode spawns at the destination entry.
- **Hotshot** - classic mode restores the destination snapshot's level/XP while
  keeping inventory and quests; fixed mode refuses the warp.
- **Fake Level Up** - classic mode advances the level with XP unchanged; fixed
  mode refuses the level-up.
- **Item duping** - classic mode copies equipped items into the inventory on the
  airlock cycle; fixed mode does not duplicate them.
- **The Routine** - the documented chain of menu glitch, hotshot, spawn warp,
  fake level and duping completes only in classic mode.

Gate: `classic_*`/`fixed_*` pairs for all seven glitches, including
`classic_spawn_warp`, `classic_hotshot`, `classic_fake_level`,
`classic_dupe` and `classic_routine`.
Evidence: `docs/evidence/s14/bug-parity.log`.

## 8. Retail trace coverage and differential

`check_retail_traces.py` reads the recorded s4 retail semantic traces and
asserts every action ID the retail engine executed is implemented:

- **91,967 events** parsed from the retail semantic traces;
- **127 observed action IDs**;
- **772 implemented action-table entries**;
- **0 missing**.

`kotor_retail_differential` then records engine semantic traces for the
new-game path and the Taris, Taris Lower City, Unknown World autosave and
Unknown World Game1 retail saves, and diffs them against every recorded retail
path (`live-02` through `live-05`, plus the four save loads):

- engine traces: **7,763** (`end_m01aa`), **1,045** (`tar_m02af`), **2,499**
  (`tar_m03aa`), **1,682** (`unk_m41ad`) and **1,240** (`unk_m41ab`) actions;
  **9 / 0 / 0 / 0 / 0** dialogue choices and one transition each;
- retail traces: **110 / 124 / 124 / 102** live action IDs plus **54 / 54 / 57
  / 43** for the four saves, **152 unique** across the set;
- retail module script actions: **93 / 108 / 108 / 89** (`end_m01aa`),
  **27** (`tar_m02af`), **48** (`tar_m03aa`), **43** (`unk_m41ad`) and **30**
  (`unk_m41ab`);
- executed across the playthroughs + drained VM corpus (357 action IDs) +
  fifteen branch-gated probes: **374** (`end_m01aa`) and **372** (later
  modules) action IDs;
- **0 missing execution** and **0 missing implementation** for every path, and
  every engine/save trace passes the module-identity check.

The provenance of each trace (recorded run, raw event file, save fixture and
SHA-256) is in `docs/evidence/s14/retail-trace-sources.json`.

Both the module-restricted and full retail sets are checked; menu/chargen IDs
are reported but no longer excluded. Fifteen branch-gated actions are recorded
with phase `probe` (talent/AI, perception/movement, combat-result and
persistent-object queries) because their branches are state-gated. Each save
trace carries a module header, and the checker rejects a trace whose recorded
module does not match the engine module.

Evidence: `docs/evidence/s14/retail-trace.log`,
`docs/evidence/s14/retail-trace-coverage.json`,
`docs/evidence/s14/engine-semantic-end_m01aa.jsonl`,
`docs/evidence/s14/retail-differential.json`,
`docs/evidence/s14/retail-differential-engine-end_m01aa.log`.

## 9. P6 baseline maintenance

`check_p6_content.py` now refuses to overwrite an existing baseline without
`--accept-baseline-change`, and validates the baseline schema before
comparison. The 117-module baseline is unchanged and still green.

## 10. Regression

- **CTest: 51/51 passed** (847.96 s).
- **117/117 modules**, 0 failures, 0 warnings, 11,887 entities, 51 encounters.
- **VM corpus: 8,666/8,666**, 0 mismatches, 303,616 ACTION calls.
- **GL/Vulkan parity: 0.0000 mean abs channel diff this run**, PASS (tolerance
  gate).
- **Python tools: 143 tests, 7 skips**, 0 failures.
- **Install sentinel: 15,380 files, 0 session changes.**

## 11. Residual boundaries

- The shipped `end_m01aa` GIT references `end_carth001`, but that DLG is absent
  from the VFS; retail cannot start it either. The matrix counts it as exactly
  one known data anomaly and fails on any other missing conversation.
- External `.rsv` support covers the module-ERF form used by the gate. A retail
  `.rsv` with a different container layout would need its own fixture.
- Retail traces now cover the new-game/Endar Spire path, Taris, Taris Lower City
  and two Unknown World saves (5 of 117 modules). A console-warp attempt using
  three input paths did not open the retail console in this headless profile
  (`docs/evidence/s14/console-warp-attempts.log`), so the 112 uncovered modules
  require additional retail saves or traces. Quests without a recorded retail
  session rely on the all-117-module conversation matrix and the all-DLG
  corpus; the manifest accepts additional recorded traces without code changes.
- Bug-parity models the seven documented glitches at their observable state
  transitions (menu, load-zone proximity, party spawn coordinates, hotshot
  snapshot restore, XP-free level-up, airlock duplication and the Routine
  chain); it does not replay full multi-hour quest sequences for each glitch.
- Non-default `CamVidEffect` semantics are not implemented because the shipped
  KotOR 1 corpus contains none; the runtime now rejects them explicitly.

## Evidence index

- `docs/recon/62_BUG_PARITY.md`
- `docs/recon/61_SPRINT14_CORRECTNESS.md`
- `engine/tests/fixtures/retail_traces.json`
- `tools/harness/semantic_from_events.py`
- `docs/evidence/s14/ctest.log`
- `docs/evidence/s14/differential.log`
- `docs/evidence/s14/save-matrix.log`
- `docs/evidence/s14/dialogue-corpus.log`
- `docs/evidence/s14/faction.log`
- `docs/evidence/s14/localization.log`
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
- `docs/evidence/s14/all-modules.log`
- `docs/evidence/s14/vm-corpus.log`
- `docs/evidence/s14/parity.log`, `parity-gl.png`, `parity-vk.png`
- `docs/evidence/s14/tools-tests.log`
- `docs/evidence/s14/install-sentinel.log`
