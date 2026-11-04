# Sprint 13 - audio, media, GUI, localization, mods and minigame parity

Status: **complete - gate S13 met (2026-11-04)**. Sprint 12 closed animation/skinning, weapons, cutscene
cameras and visual FX. Sprint 13 adds the runtime audio layer: shipped WAV/MP3
streams, area music and ambience, UTS sound emitters, dialogue voice-over, a
deterministic mixer with 3D panning/attenuation, EAX-style room reverb, and the
script sound/music actions. It also closes SSF sound sets/footsteps/barks,
Bink container/pixel decode and presentation, LIP visemes, GUI screens and the
three minigame state machines.

Baseline: Sprint 12 `36/36` CTest, 117/117 modules, VM 8,666/8,666, scene
parity 0.0449, Python 143 tests / 7 skips, install unchanged.

## Gate S13

1. **Decode** - RIFF PCM (8/16-bit, mono/stereo), IMA ADPCM (format 0x11),
   MP3, and the verified KotOR `0x1d6` stream prefix. All shipped
   `streammusic`, `streamsounds` and `streamwaves` files decode to finite PCM.
2. **Mixer** - 48 kHz stereo float mixer with music/ambient/effects/voice
   buses, per-voice volume, pitch, pan, loop and deterministic sample stepping.
3. **3D and EAX** - listener position/yaw, distance attenuation, stereo pan,
   and `soundeax.2da` room presets applied to the effects bus.
4. **Area audio** - GIT `AreaProperties` resolved through `ambientmusic.2da`,
   `ambientsound.2da` and `soundeax.2da`; music, ambient loops and volumes.
5. **Sound objects** - GIT `SoundList` + UTS templates: sound refs, position,
   elevation, min/max distance, volume, loop/positional/continuous flags.
6. **Voice-over** - DLG entry/reply `Sound` refs play on the voice bus from
   `streamwaves` (with the localized directory layout supported by the file
   index).
7. **Script bridge** - `PlaySound`, `SoundObjectPlay/Stop/SetVolume/
   SetPosition`, `MusicBackgroundPlay/Stop/ChangeDay/ChangeNight/Get*`,
   `AmbientSoundPlay/Stop/ChangeDay/ChangeNight/Set*Volume`.
8. **Sound sets** - SSF V1.1 (type 2060) parses; UTC `SoundSetFile` maps
   through `soundset.2da`; SSF StrRefs resolve through `dialog.tlk` sound
   resrefs; creature selection/battle/pain/attack/death slots and
   `BarkString` play; `appearance.footsteptype` drives
   `footstepsounds.2da` surface steps.
9. **Movies, lip sync and facial rig** - Bink 1 containers validate on all 61
   shipped movies (header, frame table, dimensions, frame clock, script movie
   actions and queues); native FFmpeg pixel decode produces the recorded
   640x272 RGBA frame (SHA-256 `301f27f0...f1430`) and presents GL/Vulkan movie
   frames; LIP V1.0 parses all 18,206 lip resources; `visemes.2da` drives a
   procedural jaw/mouth/brow rig on the shipped head bones from
   `visemeAt(time)`, with real head-geometry deformation and a debug rig view;
   Bink Audio (DCT) decodes all 5.3M frames and matches the FFmpeg PCM
   SHA-256, with the movie-bus cursor locked to the video clock, seek and stop.
10. **GUI screens and interaction** - all 86 unique `.gui` screens (91
    resources including patch.erf), 2,383 controls, 8 known control types and
    640x480 extents parse; `GuiSystem` adds hit-testing, per-control state,
    button/checkbox/listbox actions, slider/scrollbar/progress clamping, a
    screen stack and panel actions; `--gui-screen` renders the interactive
    tree and `--gui-action` drives a deterministic control.
11. **Localization** - TLK V3.0 language ids, `--language <id|name>`,
    `dialog_<id>.tlk` fallback and mismatch retention; localized voice paths
    `streamwaves/<module>/<speaker>/<resref>.wav`; 117 `_loc.mod` archives plus
    `localization.mod` (118 localized LIPs) parsed and asserted.
12. **Mods/Override VFS** - explicit five-pass first-match overlay (flat
    Override/streams, patch/textures ERF, module RIMs, `.mod`/lips/texture
    packs, KEY/BIF), overlay-root support, source provenance and TSLPatcher-
    style 2DA row/column merges.
13. **Minigames** - deterministic pazaak (40-card deck, four side cards,
    first to two sets, `PlayPazaak`/`GetLastPazaakResult` and wager
    settlement), swoop (throttle/boost/brake, 3 x 3,000 m laps) and turret
    (five approaching targets, shots/hits/score, arrival damage) state
    machines with seeded transcripts.
14. **KotOR 2 world loading** - profile-gated K2 geometry (token `0x416310`,
    340-byte trimesh header, MDX offset `+332`) and `_dlg.erf` mounting;
    `001EBO`, `007EBO` and `002EBO` load through the same path with zero
    warnings, and `001EBO` builds 18 rooms / 175 entities / 70 meshes.
15. **Faction reputation** - 21x21 `repute.2da` matrix, `GetReputation` /
    `AdjustReputation` and `GetFactionAverageReputation`, faction-derived GIT
    hostility, and FAC `REPUTE` parse/rebuild round-trip (405 non-100 pairs).
16. **P6 all-area content** - `kotor_p6_content` sweeps all 117 modules via
    per-module JSON (`--content-stats`) and asserts 106 unique areas, 11,887
    entities, 7,677 entity meshes, 991 triggers, 51 encounters, 1,137 cameras,
    1,103 sounds, 20,497 script hooks and 0 warnings against a committed
    baseline.
17. **Device** - SDL3 audio stream pulls the mixer in live frames; headless
    tests exercise the same mixer without a device.
18. **Regression** - 45/45 CTest, 117/117 modules with 0 warnings, VM
    8,666/8,666, scene parity PASS, Python 143 tests / 7 skips, install
    sentinel 0 session changes.
19. **Evidence and docs** - audio/media/localization/VFS/K2/faction/P6/GUI/minigame tests, phase log,
    module/VM/parity/tools/install logs, report and technical note.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s13-01-decode | WAV PCM/IMA, MP3, KotOR prefix | `engine/src/audio/Audio.cpp` |
| s13-02-mixer | buses, 3D, loop, EAX reverb | `engine/src/audio/Audio.*` |
| s13-03-scene | AreaProperties, sound tables, UTS emitters | `engine/src/kotor/Scene.cpp` |
| s13-04-system | decode cache, music/ambient/voice/objects | `engine/src/game/AudioSystem.*` |
| s13-05-bridge | dialogue voice and script actions | `engine/src/game/GameSession.cpp` |
| s13-06-device | SDL3 output stream | `engine/src/audio/Output.*`, `Application.cpp` |
| s13-07-soundsets | SSF, soundset mapping, footsteps and barks | `engine/src/audio`, `game/*` |
| s13-08-media | Bink container/pixels/audio, LIP visemes and facial rig | `engine/src/kotor/Media.*`, `BinkDecoder.*`, `engine/src/game/FacialRig.*`, `GameSession.*` |
| s13-09-gui | `.gui` parser, corpus assertions, interaction and renderer | `engine/src/kotor/Gui.*`, `engine/src/game/GuiSystem.*`, `Application.cpp` |
| s13-10-minigames | pazaak/swoop/turret state machines and gate | `engine/src/game/Minigames.*`, `Application.cpp` |
| s13-11-localization | TLK language and localized voice/LIP paths | `engine/src/kotor/Localization.*`, `GameSession.*` |
| s13-12-mods | five-pass VFS, Override root and 2DA merge | `engine/src/kotor/Install.*`, `TwoDa.*`, `Application.cpp` |
| s13-13-k2 | K2 profile gate, geometry layout and `_dlg.erf` | `engine/src/kotor/Profile.h`, `Geometry.cpp`, `Install.cpp`, `Application.cpp` |
| s13-14-factions | repute matrix, actions, hostility and FAC round-trip | `engine/src/game/Factions.*`, `GameSession.*`, `SaveGame.*` |
| s13-15-p6 | all-area content audit, baseline and CTest gate | `engine/scripts/check_p6_content.py`, `engine/tests/fixtures/p6_content_baseline.json` |
| s13-16-evidence | gate, report, recon note, logs | `engine/CMakeLists.txt`, `docs/` |

## Next sprint

Gate S13 covers audio, Bink/LIP media, Bink audio, the facial rig, GUI
interaction, localization, mods/Override, KotOR 2 world loading, faction
reputation and the P6 all-area content checklist. Sprint 14
([`docs/SPRINT14.md`](SPRINT14.md)) covers P6 correctness and the residuals,
Sprint 15 ([`docs/SPRINT15.md`](SPRINT15.md)) the release candidate, Sprint 16
([`docs/SPRINT16.md`](SPRINT16.md)) the first remaster stage, Sprint 17
([`docs/SPRINT17.md`](SPRINT17.md)) the remaining remaster/modding/platform
work, and Sprint 18 ([`docs/SPRINT18.md`](SPRINT18.md)) KotOR 2 gameplay
parity as the final sprint.

## Running the gate

```powershell
cd engine
cmd /c "scripts\build.bat msvc-release"
.\build\msvc-release\kotor_engine.exe --module end_m01aa --audio-test `
  --audio-script "tests\fixtures\audio_script_test.ncs" --null --headless
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $ctest --preset msvc-release --output-on-failure
```
