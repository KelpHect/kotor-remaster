# Recon 51 - audio streams, mixer and EAX approximation

Status: Sprint 13 implementation note. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## 1. Shipped stream layout

| Directory | Files | Formats |
|---|---:|---|
| `streamwaves` | 13,799 | RIFF PCM, 8-bit mono 22,050 Hz |
| `streamsounds` | 970 | 0x1d6 prefix + RIFF PCM, 16-bit mono 22,050 Hz (948); stereo 18; 44,100 Hz 4 |
| `streammusic` | 119 | RIFF PCM 8-bit mono 22,050 Hz (68); IMA ADPCM stereo 44,100 Hz (47); PCM stereo 44,100/22,050 Hz (4) |

Every `streamsounds` file starts with a 470-byte (`0x1d6`) KotOR prefix; the
embedded `RIFF/WAVE` header is at +470. Some `streammusic` files carry a RIFF
header with `data` size 0, followed by an ID3 tag and MP3 frames; the stale
fmt block is PCM but the payload is MPEG-1 Layer III. The loader scans for the
first RIFF/WAVE header, skips the ID3 tag when present, and falls back to the
MP3 decoder. `BIF` type 4 WAVs (e.g. `gui_click`) load directly.

The embedded `dr_mp3` (public domain/MIT-0, SHA-256
`F5C8020EEEBB79877184432EFDD2651F24869F5BB8F379D45450FD0AED1EE915`)
decodes the MP3 payload. PCM 8/16-bit and IMA ADPCM are first-party decoders.

## 2. Area audio

`GIT` root `AreaProperties` carries:

- `MusicDay`, `MusicNight`, `MusicBattle` - row ids in `ambientmusic.2da`;
- `AmbientSndDay`, `AmbientSndNight` - row ids in `ambientsound.2da`;
- `AmbientSndDayVol`, `AmbientSndNitVol` - 0-100;
- `EnvAudio` - row id in `soundeax.2da`;
- `MusicDelay` - milliseconds.

`ambientmusic.resource` is a `streammusic` resref. `ambientsound.resource` is
an ambient loop resref. `soundeax.label` selects one of 24 presets from
`GENERIC` through `UNDERWATER`/`NOREVERB`.

`GIT SoundList` stores `TemplateResRef` plus position. The UTS template
(type 2035) supplies `Sounds[].Sound`, `Active`, `Continuous`, `Looping`,
`Positional`, `Random`, `Elevation`, `MinDistance`, `MaxDistance`, `Volume`,
`Interval` and `IntervalVrtn`.

## 3. Mixer

`audio::Mixer` is a 48 kHz stereo float mixer with four buses:

- Music - looping area track, 0.75 default;
- Ambient - looping area ambience, 0.65 default;
- Effects - UTS sound objects and script `PlaySound`;
- Voice - DLG entry/reply voice-over.

Each voice stores a shared decoded buffer, cursor, loop flag, pitch, volume,
pan and optional world position. Positional gain is
`clamp((maxDistance-distance)/(maxDistance-minDistance), 0, 1)^2`; pan uses the
listener yaw right vector `(sin yaw, -cos yaw)`. Looping wraps the cursor;
non-looping voices retire at the end. The mixer is deterministic: the same
buffer, cursor and call count produce identical samples.

## 4. EAX approximation

`soundeax` presets map to `{delayMs, feedback, wet}`. The effects bus runs
through a damped stereo echo:

```text
delayed = delayLine[index]
delayLine[index] = input + delayed * feedback
output = input * (1 - wet) + delayed * wet
```

`NOREVERB` disables the wet path. This is an explicit approximation, not a
claim of Miles/EAX bit parity.

## 5. Integration

`GameSession` owns `AudioSystem`. Area load resolves music/ambient, sets the
reverb preset and starts loops. `update` moves the listener with the player.
Dialogue entries/replies call `playVoice` for non-empty `Sound` fields.
Script actions map to the audio API; `SoundObject*` actions resolve the script
object id back to the GIT SoundList entity.

## 6. Validation

`--audio-test` asserts:

- area music `mus_theme_sith`, ambient `al_en_starforge`, EAX `HALLWAY`;
- 45 sound emitters and a playable UTS object;
- MP3 (`as_el_alert`) and PCM (`gui_click`) decode plus 48 kHz stereo resample;
- music/ambient voices and a non-silent area mix;
- mixer RMS, distance attenuation, stereo pan, EAX wet path, stop-all and loop
  wrap;
- `audio_script_test.ncs` `PlaySound`, `MusicBackgroundChangeDay`,
  `AmbientSoundChangeDay` and `AmbientSoundSetDayVolume`.

## 7. Limits

Bink movies, LIP lip sync, SSF sound sets/footsteps/barks, full `.gui`,
minigames, localization, mods/Override precedence, KotOR 2 world loading,
faction reputation and the P6 checklist remain future sprints.
