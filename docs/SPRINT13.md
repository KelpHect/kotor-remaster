# Sprint 13 - audio, music, voice and EAX approximation

Status: **complete - gate S13 met (2026-11-04)**. Sprint 12 closed animation/skinning, weapons, cutscene
cameras and visual FX. Sprint 13 adds the runtime audio layer: shipped WAV/MP3
streams, area music and ambience, UTS sound emitters, dialogue voice-over, a
deterministic mixer with 3D panning/attenuation, EAX-style room reverb, and the
script sound/music actions.

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
9. **Device** - SDL3 audio stream pulls the mixer in live frames; headless
   tests exercise the same mixer without a device.
10. **Regression** - 37/37 CTest, 117/117 modules with 0 warnings, VM
    8,666/8,666, scene parity PASS, Python 143 tests / 7 skips, install
    sentinel 0 session changes.
11. **Evidence and docs** - audio test, phase log, module/VM/parity/tools/install
    logs, report and technical note.

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
| s13-08-evidence | gate, report, recon note, logs | `engine/CMakeLists.txt`, `docs/` |

## Outside this sprint

- Bink movie playback and LIP lip sync.
- Full `.gui` screens, minigames, localization, mods/Override precedence and
  KotOR 2 world loading.
- Faction reputation simulation and the P6 content checklist.

## Running the gate

```powershell
cd engine
cmd /c "scripts\build.bat msvc-release"
.\build\msvc-release\kotor_engine.exe --module end_m01aa --audio-test `
  --audio-script "tests\fixtures\audio_script_test.ncs" --null --headless
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $ctest --preset msvc-release --output-on-failure
```
