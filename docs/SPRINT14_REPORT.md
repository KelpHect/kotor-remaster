# Sprint 14 report - P6 correctness and residual gaps

Status: **complete - gate S14 met (2026-11-04)**. Sprint 13 closed the
runtime/media/GUI/localization/mods/K2-world/faction/P6-content systems. Sprint
14 closed the correctness gaps that kept KotOR 1 from being end-to-end
playable and added the differential/save-matrix evidence.

## 1. Build and test result

```text
100% tests passed, 0 tests failed out of 48
Total Test Time (real) = 277.76 sec
```

New CTest targets: `kotor_save_matrix`, `kotor_differential`,
`kotor_dialogue_corpus`. The existing 45 targets still pass.

## 2. Differential playthrough gate

`--differential-test` combines:

- **Retail save differential** - seven retail save fixtures (quicksave,
  autosave, Game1, synthetic, vanilla, Taris, Taris Lower City) load, match
  their saved gold/inventory/journal/party/position and object state, then
  save and reload through the engine.
- **Dialogue/cutscene corpus** - **1,043 unique shipped DLGs**, **22,572
  entries**, **25,300 replies**, **5,900 scripts**, **6,715 sounds** and
  **22 camera models**, all parsed with zero failures.
- **Deterministic Endar Spire playthrough** - Trask dialogue (9 lines), trigger
  `end_trig02`, locker use, SithSoldier combat (faction-correct), module
  transition `end_m01aa -> end_m01ab`, and save/load of `end_m01ab`.

Evidence: `docs/evidence/s14/differential.log`.

## 3. Save/load matrix

`--save-matrix-test` covers **7/7** fixtures:

- loads each slot and matches module, gold, inventory, journal, party,
  companions and position;
- applies saved object states (position, dead, HP, door open/locked, placeable
  loot) with duplicate-tag handling;
- writes a new save, verifies the nested GIT has **no `UseTemplates` field**
  (flattened state), then reloads and re-checks gold/inventory/journal/party.

Evidence: `docs/evidence/s14/save-matrix.log`.

## 4. TSLPatcher TLK merge

`Tlk::merge(base, patch, name, replaceCount)` supports:

- overwrite of the first `replaceCount` entries;
- empty text/sound fields keeping the base value;
- append of the remaining patch entries;
- base TLK unchanged.

Gate: `tlk_merge_size`, `tlk_merge_overwrite`, `tlk_merge_sound_only`,
`tlk_merge_append`, `tlk_merge_base_unchanged`.
Evidence: `docs/evidence/s14/localization.log`.

## 5. Faction parent inheritance

`FactionSystem` now tracks explicit cells separately from inherited values.
A child faction with `FactionParentID` falls back to its parent row (source and
target), and `saveFac` writes only explicit cells while preserving parents.
Gate: `fac_parent_parse`, `fac_parent_id`, `fac_parent_source`,
`fac_parent_target`, `fac_parent_friendly`, `fac_parent_set`,
`fac_parent_set_value`, `fac_parent_save`, `fac_parent_roundtrip`.
Evidence: `docs/evidence/s14/faction.log`.

## 6. CamVidEffect audit

The dialogue corpus has **0** non-default `CamVidEffect` values across all
1,043 DLGs. Runtime dialogue now records and warns on a non-default value
instead of silently ignoring it. Gate: `dialogue corpus: camvid_defaults=1`.

## 7. P6 baseline maintenance

`check_p6_content.py` now refuses to overwrite an existing baseline without
`--accept-baseline-change`, and validates the baseline schema before
comparison. The 117-module baseline is unchanged and still green.

## 8. Regression

- **CTest: 48/48 passed** (277.76 s).
- **117/117 modules**, 0 failures, 0 warnings, 11,887 entities, 51 encounters.
- **VM corpus: 8,666/8,666**, 0 mismatches, 303,616 ACTION calls.
- **GL/Vulkan parity: 0.0000**, PASS.
- **Python tools: 143 tests, 7 skips**, 0 failures.
- **Install sentinel: 15,380 files, 0 session changes.**

## 9. Residual boundaries

- The shipped install contains no external `<module>.rsv`; the save matrix
  covers the seven shipped fixtures. Mod-only `.rsv` shadowing remains an
  install-hazard case for a future fixture.
- A corrupt cached `EventQueue` is not read by the engine; the gate cannot
  produce a corrupted cached module without a fixture. The loader's structural
  save validation remains the safety boundary.
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
- `docs/evidence/s14/all-modules.log`
- `docs/evidence/s14/vm-corpus.log`
- `docs/evidence/s14/parity.log`, `parity-gl.png`, `parity-vk.png`
- `docs/evidence/s14/tools-tests.log`
- `docs/evidence/s14/install-sentinel.log`
