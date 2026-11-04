# Sprint 14 - media renderer, facial rig and the remaining parity systems

Status: **planned**. Sprint 13 closed audio, SSF sound sets, Bink containers/
movie state and LIP viseme timelines. Sprint 14 covers the renderer/facial
layer and the systems that were previously listed as future work.

## Why this is a separate sprint

- **Bink pixels**: the 61 shipped BIK files carry valid headers and frame
  tables, but KotOR uses Bink 1 video payloads. Sprint 13 validates the
  container and drives the movie clock; a pixel decoder/presenter is a media
  renderer. FFmpeg 9.0.2 decodes `01a.bik` to a 640x272 RGBA frame (696,320
  bytes, SHA-256 `301f27f0...f1430`, preview
  `docs/evidence/s13/bik-oracle.png`), so the pixel oracle is established.
- **Facial morphs**: `visemes.2da` has 16 rows mapping viseme ids to animation
  names (`Silence`, `Eat`, `If`, ...). A scan of all 107 `heads.2da` rows (106 parsed models)
  finds **0** head models carrying any of those clips (and no morph-target
  data); shipped heads carry only `pause1`/`pause2`-style clips. Sprint 13
  therefore exposes the deterministic LIP timeline (`lipTime`, `lipViseme`)
  but cannot invent facial deformation without an engine facial rig.

## Gate S14

1. **Bink pixel renderer** - decode Bink 1 frames (FFmpeg/libavcodec or a
   first-party decoder), present at the header frame clock, sync movie audio,
   and keep the container/clock contract from Sprint 13.
2. **Facial rig / lip sync application** - map `visemes.2da` rows to rig
   controls, drive them from `LipSync::visemeAt`, and support the head/body
   model setup without corrupting the existing animation state.
3. **Full `.gui` screens** - parse and render the shipped 84 GUI resources,
   including controls, fonts and screen transitions.
4. **Minigames** - pazaak, swoop and turret state machines with the shipped
   rules/content.
5. **Localization** - TLK language selection and localized path handling.
6. **Mods/Override precedence** - five-pass VFS overlay and TSLPatcher-style
   merges.
7. **KotOR 2 world loading** - profile-gated K2 resources and world data.
8. **Faction reputation and P6** - faction simulation and the all-area content
   checklist.
9. **Regression** - full CTest, 117-module sweep, VM parity, GL/Vulkan parity,
   Python suite and install sentinel.

## Evidence carried into Sprint 14

- `docs/evidence/s13/media.log` - 61 BIK containers, 18,206 LIP timelines,
  107 head rows / 106 models with 0 viseme clips.
- `docs/evidence/s13/bik-oracle.txt`, `bik-oracle.png` - FFmpeg Bink frame
  oracle.
- `docs/recon/51_AUDIO.md` - stream, mixer, sound-set and media formats.
