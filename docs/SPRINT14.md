# Sprint 14 - Bink audio and the remaining parity systems

Status: **planned**. Sprint 13 closed audio, SSF sound sets, Bink containers,
native Bink pixel decode/presentation, LIP viseme timelines, the procedural
facial rig, GUI screens and the pazaak/swoop/turret state machines. Sprint 14
covers Bink audio and the systems that remain.

## Why this is a separate sprint

- **Bink audio**: the native decoder presents video frames only. The Bink
  audio tracks need a synchronized audio path locked to the video clock.

## Gate S14

1. **Bink audio sync** - decode the Bink audio tracks and keep them locked to
   the video frame clock.
2. **GUI interaction/state** - wire control actions, screen transitions and
   panel state on top of the Sprint 13 parser/renderer.
3. **Localization** - TLK language selection and localized path handling.
4. **Mods/Override precedence** - five-pass VFS overlay and TSLPatcher-style
   merges.
5. **KotOR 2 world loading** - profile-gated K2 resources and world data.
6. **Faction reputation and P6** - faction simulation and the all-area content
   checklist.
7. **Regression** - full CTest, 117-module sweep, VM parity, GL/Vulkan parity,
   Python suite and install sentinel.

## Evidence carried into Sprint 14

- `docs/evidence/s13/media.log` - 61 BIK containers, native pixel decode with
  the recorded SHA-256, 18,206 LIP timelines, 107 head rows / 106 models with
  0 viseme clips.
- `docs/evidence/s13/bik-oracle.txt`, `bik-oracle.png` - FFmpeg Bink frame
  oracle.
- `docs/evidence/s13/movie-gl.png`, `movie-vk.png`, `movie-frame.png` - movie
  presentation on both backends and the decoded frame itself.
- `docs/evidence/s13/facial-roar.png` - viseme 13 driving the facial rig.
- `docs/recon/51_AUDIO.md`, `docs/recon/54_FACIAL_RIG.md` - stream/mixer and
  viseme rig details.
