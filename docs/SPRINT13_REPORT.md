# Sprint 13 report - audio, music, voice and EAX approximation

Status: **complete - gate S13 met (2026-11-04)**. Sprint 12 closed
animation/skinning, weapons, cutscene cameras and visual FX. Sprint 13 adds
the runtime audio layer: shipped WAV/MP3 streams decode, area music/ambience
and UTS sound emitters load, dialogue voice-over plays, a deterministic mixer
provides 3D attenuation/pan and EAX-style reverb, and the script sound/music
actions are bridged.

## 1. Build and test result

```text
100% tests passed, 0 tests failed out of 37
Total Test time (real) = 231.71 sec
```

The new target is `kotor_audio`; all Sprint 12 targets still pass.

## 2. Stream decode

- **PCM WAV**: 8/16-bit, mono/stereo, including the shipped 8-bit 22,050 Hz
  wave corpus.
- **IMA ADPCM**: WAV format `0x11`, stereo 44.1 kHz, block predictors and
  nibble tables.
- **MP3**: embedded `dr_mp3` (public domain/MIT-0) for the music files whose
  RIFF `data` chunk is zero and whose payload is ID3 + MPEG-1 Layer III.
- **KotOR prefix**: every `streamsounds` file carries the verified
  `0x1d6`-byte prefix; the loader scans for the first `RIFF/WAVE` header.
- **BIF WAVs**: type 4 resources (`gui_click`) load through the KEY/BIF VFS.

PCM is resampled to 48 kHz stereo at load time with linear interpolation.
Decoded samples are finite; the `--audio-test` gate probes MP3, PCM and the
resampler.

## 3. Mixer and EAX

- 48 kHz stereo float mixer with music, ambient, effects and voice buses.
- Per-voice volume, pitch, pan, loop and deterministic cursor stepping.
- Listener position/yaw; distance attenuation `((max-d)/(max-min))^2` and
  stereo pan from the yaw right vector.
- IMA/MP3/PCM buffers share one playback path.
- `soundeax.2da` presets map to `{delayMs, feedback, wet}` on the effects bus;
  `NOREVERB` is dry.

## 4. Area audio and sound objects

`GIT AreaProperties` resolves:

- `MusicDay`, `MusicNight`, `MusicBattle` through `ambientmusic.2da`;
- `AmbientSndDay`, `AmbientSndNight` through `ambientsound.2da`;
- `AmbientSndDayVol`, `AmbientSndNitVol`, `MusicDelay`;
- `EnvAudio` through `soundeax.2da`.

Endar Spire resolves music `mus_theme_sith`, ambient `al_en_starforge` and
EAX `HALLWAY`. The GIT carries **45** sound emitters; their UTS templates
supply sound refs, volume, min/max distance, loop/positional flags and
elevation. The gate starts, changes volume/position and stops a UTS object.

## 5. Dialogue and script bridge

- DLG entry/reply `Sound` fields play on the voice bus.
- `PlaySound` plays a 2D effect.
- `SoundObjectPlay/Stop/SetVolume/SetPosition` resolve the script object to a
  GIT SoundList emitter.
- `MusicBackgroundPlay/Stop/SetDelay/ChangeDay/ChangeNight/GetDay/Night/Battle`.
- `AmbientSoundPlay/Stop/ChangeDay/ChangeNight/SetDay/NightVolume`.
- `SpeakOneLinerConversation` plays the referenced voice stream.

The compiled fixture `engine/tests/fixtures/audio_script_test.ncs` calls
`PlaySound("as_el_alert")`, changes the day music to row 3, changes ambient to
row 32 and sets the ambient day volume to 40; all four assertions pass.

## 6. Device

`audio::Output` opens an SDL3 playback stream (48 kHz float stereo) and pulls
the mixer in the audio callback. Live runs log `Audio device: <name>`; headless
tests use the same mixer without a device. The output stream is destroyed
before `SDL_Quit` (the parity crash fix).

## 7. Regression

- **CTest: 37/37 passed** (231.71 s).
- **117/117 modules**, 0 failures, 0 warnings, 11,887 entities, 51 encounters.
- **VM corpus: 8,666/8,666**, 0 mismatches, 303,616 ACTION calls.
- **GL/Vulkan parity: 0.0449**, PASS.
- **Python tools: 143 tests, 7 skips**, 0 failures.
- **Install sentinel: 15,380 files, 0 session changes.**

## 8. Outside Sprint 13

- Bink movie playback and LIP lip sync.
- SSF sound sets, footsteps and creature barks.
- Full `.gui` screens, minigames, localization, mods/Override precedence and
  KotOR 2 world loading.
- Faction reputation simulation and the P6 content checklist.

## Evidence index

- `docs/evidence/s13/audio.log`
- `docs/evidence/s13/ctest.log`
- `docs/evidence/s13/all-modules.log`, `all-modules.json`
- `docs/evidence/s13/vm-corpus.log`, `vm-parity.json`
- `docs/evidence/s13/parity.log`, `parity-gl.png`, `parity-vk.png`
- `docs/evidence/s13/tools-tests.log`
- `docs/evidence/s13/install-sentinel.log`
- Fixture: `engine/tests/fixtures/audio_script_test.nss/.ncs`
- Technical note: [docs/recon/51_AUDIO.md](recon/51_AUDIO.md)
