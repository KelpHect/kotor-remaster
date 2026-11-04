# Sprint 14 - KotOR 2, factions and the P6 checklist

Status: **planned**. Sprint 13 closed audio, SSF sound sets, Bink containers,
native Bink pixel decode/presentation, Bink Audio sync, LIP viseme timelines,
the procedural facial rig, GUI screens/interaction, localization,
mods/Override precedence and the pazaak/swoop/turret state machines. Sprint 14
covers the systems that remain.

## Why this is a separate sprint

- **KotOR 2 world loading** is a profile-gated content layer beyond the KotOR 1
  runtime slice.
- **Faction reputation and P6** are the remaining content/simulation layers.

## Gate S14

1. **KotOR 2 world loading** - profile-gated K2 resources and world data.
2. **Faction reputation and P6** - faction simulation and the all-area content
   checklist.
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
- `docs/recon/51_AUDIO.md`, `docs/recon/54_FACIAL_RIG.md` - stream/mixer and
  viseme rig details.
