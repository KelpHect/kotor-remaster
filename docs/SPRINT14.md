# Sprint 14 - P6 correctness and residual gaps

Status: **implementation complete - gate S14 met (120 paths, 117 of 117
modules)**. All 117 modules have recorded retail entry traces; quest dialogue
branches are covered by the all-DLG and VM matrices. Sprint 13 closed audio, SSF
sound sets, Bink containers, native Bink pixel decode/presentation, Bink Audio
sync, LIP viseme timelines, the procedural facial rig, GUI
screens/interaction, localization, mods/Override precedence, KotOR 2 world
loading, faction reputation, the P6 all-area content checklist and the
pazaak/swoop/turret state machines. Sprint 14 closed the P6 correctness gaps.
See [`SPRINT14_REPORT.md`](SPRINT14_REPORT.md).

## Why this is a separate sprint

The P6 content checklist is green, but content presence is not behaviour
parity. Sprint 14 measures retail-vs-engine behaviour and closes the known
correctness gaps: the save/load matrix, the all-module playthrough matrix,
retail trace action coverage, bug-parity mode, TSLPatcher TLK merges, faction
parent inheritance, the `CamVidEffect` audit and baseline maintenance.

Exit criterion: the **KotOR 1 campaign is feature-complete and end-to-end
playable**. Sprint 15 then certifies the release candidate; it adds no
gameplay. KotOR 2 gameplay is not covered here.

## Gate S14

Status: all items met, including full 117/117-module retail trace coverage.
Evidence in `docs/evidence/s14/` and `docs/SPRINT14_REPORT.md`.

1. **Differential playthroughs** - met: the all-117-module
   playthrough matrix (780 conversations, 10,133 lines), the all-DLG corpus
   (1,043 DLGs, 67,395 links), the deterministic Endar Spire playthrough, the
   recorded retail-vs-engine semantic differential across 120 paths covering
   all 117 modules (178 retail action IDs, 0 missing) and retail trace action
   coverage (91,967 events, 127 actions, 0 missing).
2. **Save/load matrix** - met: seven retail fixtures plus external `.rsv`
   shadowing, flattened `UseTemplates`, corrupt `EventQueue` and
   reload-after-save cases.
3. **TSLPatcher TLK merges** - met: `replace.tlk`/`append.tlk` merge,
   `Override` VFS precedence, preserved indices and a DLG StrRef integration
   test.
4. **Faction parent inheritance** - met: `FactionParentID` source/target
   fallback and FAC save/reload.
5. **CamVidEffect audit** - met: the shipped corpus has no non-default values;
   the runtime warns and rejects them, with a deterministic fixture.
6. **P6 baseline maintenance** - met: `--write-baseline` requires
   `--accept-baseline-change` and the schema is validated.
7. **Regression** - met: 51/51 CTest, 117-module sweep, VM parity, GL/Vulkan
   parity, Python suite and install sentinel.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s14-01-diff | retail/engine differential trace harness | `engine/scripts/`, `docs/evidence/s14/` |
| s14-02-saves | save/load matrix and corrupt-slot cases | `engine/tests/`, `docs/evidence/s14/` |
| s14-03-tlk | TLK append/replace merge and StrRef remap | `engine/src/kotor/Tlk.*`, `Install.*` |
| s14-04-factions | parent fallback and FAC round-trip | `engine/src/game/Factions.*` |
| s14-05-camvid | non-default `CamVidEffect` decision | `engine/src/kotor/Media.*`, docs |
| s14-06-baseline | baseline regeneration policy and gate | `engine/scripts/check_p6_content.py` |
| s14-07-evidence | gate, report, recon notes, logs | `engine/CMakeLists.txt`, `docs/` |

## Evidence carried into Sprint 14

- `docs/evidence/s13/p6-content.json` and
  `engine/tests/fixtures/p6_content_baseline.json` - all-area content totals
  for all 117 modules.
- `docs/evidence/s13/faction.log`, `docs/recon/59_FACTIONS.md` - repute matrix,
  script actions, hostility and FAC round-trip.
- `docs/evidence/s13/k2.log`, `docs/recon/58_KOTOR2.md` - K2 profile, geometry
  layout and three loaded areas.
- `docs/evidence/s13/vfs.log`, `docs/recon/57_MODS_VFS.md` - five-pass VFS and
  2DA merge evidence.
- `docs/evidence/s13/media.log`, `docs/recon/51_AUDIO.md`,
  `docs/recon/54_FACIAL_RIG.md` - media, lip-sync and facial rig details.
- `docs/evidence/s13/localization.log`, `docs/recon/56_LOCALIZATION.md` -
  language selection and localized voice/LIP paths.
- `docs/evidence/s13/bik-oracle.txt`, `bik-audio.txt`, `movie-*.png` - Bink
  pixel/audio oracles and presentation captures.
