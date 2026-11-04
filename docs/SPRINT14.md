# Sprint 14 - facial rig, Bink audio and the remaining parity systems

Status: **planned**. Sprint 13 closed audio, SSF sound sets, Bink containers,
native Bink pixel decode/presentation, LIP viseme timelines, GUI screens and
the pazaak/swoop/turret state machines. Sprint 14 covers the facial rig and the
systems that remain.

## Why this is a separate sprint

- **Facial morphs**: `visemes.2da` has 16 rows mapping viseme ids to animation
  names (`Silence`, `Eat`, `If`, ...). A scan of all 107 `heads.2da` rows (106 parsed models)
  finds **0** head models carrying any of those clips (and no morph-target
  data); shipped heads carry only `pause1`/`pause2`-style clips. Sprint 13
  therefore exposes the deterministic LIP timeline (`lipTime`, `lipViseme`)
  but cannot invent facial deformation without an engine facial rig.
- **Bink audio**: the native decoder currently presents video frames only.
  The Bink audio tracks need a synchronized audio path.

## Gate S14

1. **Facial rig / lip sync application** - map `visemes.2da` rows to rig
   controls, drive them from `LipSync::visemeAt`, and support the head/body
   model setup without corrupting the existing animation state.
2. **Bink audio sync** - decode the Bink audio tracks and keep them locked to
   the video frame clock.
3. **GUI interaction/state** - wire control actions, screen transitions and
   panel state on top of the Sprint 13 parser/renderer.
4. **Localization** - TLK language selection and localized path handling.
5. **Mods/Override precedence** - five-pass VFS overlay and TSLPatcher-style
   merges.
6. **KotOR 2 world loading** - profile-gated K2 resources and world data.
7. **Faction reputation and P6** - faction simulation and the all-area content
   checklist.
8. **Regression** - full CTest, 117-module sweep, VM parity, GL/Vulkan parity,
   Python suite and install sentinel.

## Evidence carried into Sprint 14

- `docs/evidence/s13/media.log` - 61 BIK containers, native pixel decode with
  the recorded SHA-256, 18,206 LIP timelines, 107 head rows / 106 models with
  0 viseme clips.
- `docs/evidence/s13/bik-oracle.txt`, `bik-oracle.png` - FFmpeg Bink frame
  oracle.
- `docs/evidence/s13/movie-gl.png`, `movie-vk.png`, `movie-frame.png` - movie
  presentation on both backends and the decoded frame itself.
- `docs/recon/51_AUDIO.md` - stream, mixer, sound-set and media formats.
