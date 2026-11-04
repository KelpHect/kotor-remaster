# Sprint 14 report - P6 correctness and residual gaps

Status: **complete - gate S14 met (2026-11-04)**. Sprint 13 closed the
runtime/media/GUI/localization/mods/K2-world/faction/P6-content systems. Sprint
14 closed the correctness gaps that kept KotOR 1 from being end-to-end
playable and added the differential/save-matrix, retail-trace and bug-parity
evidence.

## 1. Build and test result

```text
100% tests passed, 0 tests failed out of 50
Total Test Time (real) = 871.10 sec
```

New CTest targets: `kotor_save_matrix`, `kotor_differential`,
`kotor_dialogue_corpus`, `kotor_bug_parity` and `kotor_retail_trace`. The
existing 45 targets still pass.

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

`--bug-parity-test` runs the same scenario in classic and fixed modes:

- **Anywhere Menu Glitch** - classic mode opens the character panel during
  dialogue; fixed mode refuses it.
- **Displaced Load Zone** - classic mode transitions through a door from an
  arbitrary position; fixed mode requires proximity.

Gate: `classic_anywhere_menu=1`, `fixed_anywhere_menu=1`,
`classic_displaced_load=1`, `fixed_displaced_load=1`.
Evidence: `docs/evidence/s14/bug-parity.log`.

## 8. Retail trace coverage

`check_retail_traces.py` reads the recorded s4 retail semantic traces and
asserts every action ID the retail engine executed is implemented:

- **91,967 events** parsed from the retail semantic traces;
- **127 observed action IDs**;
- **772 implemented action-table entries**;
- **0 missing**.

Evidence: `docs/evidence/s14/retail-trace.log`,
`docs/evidence/s14/retail-trace-coverage.json`.

## 9. P6 baseline maintenance

`check_p6_content.py` now refuses to overwrite an existing baseline without
`--accept-baseline-change`, and validates the baseline schema before
comparison. The 117-module baseline is unchanged and still green.

## 10. Regression

- **CTest: 50/50 passed** (871.10 s).
- **117/117 modules**, 0 failures, 0 warnings, 11,887 entities, 51 encounters.
- **VM corpus: 8,666/8,666**, 0 mismatches, 303,616 ACTION calls.
- **GL/Vulkan parity: 0.0815 mean abs channel diff**, PASS (tolerance gate).
- **Python tools: 143 tests, 7 skips**, 0 failures.
- **Install sentinel: 15,380 files, 0 session changes.**

## 11. Residual boundaries

- The shipped `end_m01aa` GIT references `end_carth001`, but that DLG is absent
  from the VFS; retail cannot start it either. The matrix counts it as exactly
  one known data anomaly and fails on any other missing conversation.
- External `.rsv` support covers the module-ERF form used by the gate. A retail
  `.rsv` with a different container layout would need its own fixture.
- Retail trace evidence is action-ID coverage from the recorded s4 semantic
  traces, paired with the all-117-module playthrough and all-DLG matrices; it
  is not a separately recorded retail trace for every quest.
- Bug-parity mode currently models **Anywhere Menu Glitch** and **Displaced Load
  Zone**. Spawn Warps, Hotshot, The Routine, Fake Level Up and item duping are
  documented but not modeled.
- Non-default `CamVidEffect` semantics are not implemented because the shipped
  KotOR 1 corpus contains none; the runtime now rejects them explicitly.

## Evidence index

- `docs/recon/61_SPRINT14_CORRECTNESS.md`
- `docs/evidence/s14/ctest.log`
- `docs/evidence/s14/differential.log`
- `docs/evidence/s14/save-matrix.log`
- `docs/evidence/s14/dialogue-corpus.log`
- `docs/evidence/s14/faction.log`
- `docs/evidence/s14/localization.log`
- `docs/evidence/s14/bug-parity.log`
- `docs/evidence/s14/retail-trace.log`
- `docs/evidence/s14/retail-trace-coverage.json`
- `docs/evidence/s14/all-modules.log`
- `docs/evidence/s14/vm-corpus.log`
- `docs/evidence/s14/parity.log`, `parity-gl.png`, `parity-vk.png`
- `docs/evidence/s14/tools-tests.log`
- `docs/evidence/s14/install-sentinel.log`
