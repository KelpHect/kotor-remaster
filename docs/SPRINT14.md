# Sprint 14 - P6 correctness and residual gaps

Status: **planned**. Sprint 13 closed audio, SSF sound sets, Bink containers,
native Bink pixel decode/presentation, Bink Audio sync, LIP viseme timelines,
the procedural facial rig, GUI screens/interaction, localization,
mods/Override precedence, KotOR 2 world loading, faction reputation, the P6
all-area content checklist and the pazaak/swoop/turret state machines. Sprint
14 is the first P6 correctness sprint: it combines the differential
playthrough/save matrix with the small residual gaps that must close before
Sprint 15 release checks and the Sprint 16 remaster path. KotOR 2 gameplay
parity is deliberately the final sprint (Sprint 18), after the remaster work.

## Why this is a separate sprint

The P6 content checklist is green, but content presence is not behaviour
parity. Sprint 14 measures retail-vs-engine behaviour and closes the known
correctness gaps: TSLPatcher TLK merges, faction parent inheritance, the
`CamVidEffect` audit and baseline maintenance.

## Gate S14

1. **Differential playthroughs** - retail-vs-engine behaviour on every quest
   and cutscene path, including bug-parity mode vs fixed mode. Record event
   traces, dialogue choices, transitions and combat outcomes.
2. **Save/load matrix** - original saves and engine saves through autosave,
   quicksave, manual save, module transitions, `.rsv` shadowing,
   `UseTemplates=0`, corrupt `EventQueue` and reload-after-transition cases.
3. **TSLPatcher TLK merges** - append and replace StrRef entries, preserve
   entry indices for 2DA/GFF references, and apply the same VFS precedence as
   the existing 2DA merges.
4. **Faction parent inheritance** - honour `FactionParentID` when a child
   faction has no explicit relation, and preserve parents through FAC
   save/reload.
5. **CamVidEffect audit** - the shipped KotOR 1 corpus has no non-default
   values. Either implement the verified semantics for mods or reject them
   with an explicit warning; never silently ignore them.
6. **P6 baseline maintenance** - document and gate `--write-baseline` so the
   117-module baseline changes only with a reviewed engine/data change.
7. **Regression** - full CTest, 117-module sweep, K2 world gate, VM parity,
   GL/Vulkan parity, Python suite and install sentinel.

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
