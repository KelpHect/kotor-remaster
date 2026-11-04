# Sprint 14 - P6 all-area content parity

Status: **planned**. Sprint 13 closed audio, SSF sound sets, Bink containers,
native Bink pixel decode/presentation, Bink Audio sync, LIP viseme timelines,
the procedural facial rig, GUI screens/interaction, localization,
mods/Override precedence, KotOR 2 world loading, faction reputation and the
pazaak/swoop/turret state machines. Sprint 14 covers the remaining P6 content
checklist.

## Why this is a separate sprint

P6 is the full-game checklist from `docs/MASTER_PLAN.md`: every module, quest,
dialogue, cutscene, item, creature, encounter and localization path, plus
differential playthroughs, save/load matrix, performance, controller support
and legal review. It is a content/verification program, not a single runtime
feature.

## Gate S14

1. **P6 content checklist** - all-area content coverage and differential
   playthroughs.
2. **Release checks** - performance budgets, controller/accessibility and
   legal review.
3. **Regression** - full CTest, 117-module sweep, VM parity, GL/Vulkan parity,
   Python suite and install sentinel.

## Evidence carried into Sprint 14

- `docs/evidence/s13/media.log` - 61 BIK containers, native pixel decode with
  the recorded SHA-256, 18,206 LIP timelines, 107 head rows / 106 models with
  0 viseme clips.
- `docs/evidence/s13/bik-oracle.txt`, `bik-oracle.png` - FFmpeg Bink frame
  oracle.
- `docs/evidence/s13/bik-audio.txt` - FFmpeg Bink Audio PCM oracle.
- `docs/evidence/s13/movie-gl.png`, `movie-vk.png`, `movie-frame.png` - movie
  presentation on both backends and the decoded frame itself.
- `docs/evidence/s13/facial-roar.png` - viseme 13 driving the facial rig.
- `docs/evidence/s13/gui-state.png` - interactive mainmenu control tree.
- `docs/evidence/s13/localization.log`, `docs/recon/56_LOCALIZATION.md` -
  language selection and localized voice/LIP paths.
- `docs/evidence/s13/vfs.log`, `docs/recon/57_MODS_VFS.md` - five-pass VFS and
  2DA merge evidence.
- `docs/evidence/s13/k2.log`, `docs/recon/58_KOTOR2.md` - K2 profile, geometry
  layout and three loaded areas.
- `docs/evidence/s13/faction.log`, `docs/recon/59_FACTIONS.md` - repute matrix,
  script actions, hostility and FAC round-trip.
- `docs/recon/51_AUDIO.md`, `docs/recon/54_FACIAL_RIG.md` - stream/mixer and
  viseme rig details.
