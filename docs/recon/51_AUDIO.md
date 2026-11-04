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

## 6. Sound sets and footsteps

`soundset.2da` maps row id -> label/resref. Creature UTC `SoundSetFile` is that
row id (`65535` = none). The resref names an **SSF V1.1** resource (type 2060):

```text
0x00 char[8] "SSF V1.1"
0x08 u32     lead = 12
0x0C u32[]   StrRefs to EOF (0xFFFFFFFF = empty)
```

Shipped files are 172 bytes = 40 slots. A slot StrRef indexes `dialog.tlk`; the
TLK 40-byte entry carries a 16-byte sound resref at +4 (e.g.
`n_sithsoldr_atk1`). Slot ids: battle cries 0-5, select 6-8, attack grunts
9-11, pain 12-13, low health 14, dead 15, critical 16, target immune 17,
mines 18-19, stealth/search/unlock 20-24, party 25-26, poisoned 27.

`appearance.footsteptype` (0 Soft, 1 Hard, 2 Droid, 3 Droid_Heavy) selects a
`footstepsounds.2da` row. Surface id selects the column family:
dirt(1), grass(3), stone(4), wood(5), water(6/11), carpet(9), metal(10).
The 0/1/2 variant is a deterministic footstep counter.

## 7. Bink and LIP media

Bink 1 header (all 61 shipped movies, version `i`):

```text
0x00 "BIK" + version
0x04 filesize-8
0x08 frame count, 0x0C largest frame, 0x10 duplicate frame count
0x14 width, 0x18 height, 0x1C frame clock (value/1000)
0x38 (frames+1) u32 frame offsets, EOF sentinel == filesize
```

The engine validates every header and frame table, stores the playback clock
and implements `PlayMovie`/`QueueMovie`/`PlayMovieQueue`/`IsMoviePlaying`.
Dimensions are 640x272 (53), 640x360 (5), 640x480 (3); pixel decoding is the
Sprint 14 media-renderer step with FFmpeg as the documented oracle.

LIP V1.0 (type 3004, 18,206 resources in 123 `lips/*.mod`):

```text
0x00 "LIP V1.0"
0x08 f32 duration
0x0C u32 frame count
0x10 count x (f32 time, u8 viseme 0..15)
```

The parser enforces exact size, monotonic times, first 0, last == duration and
viseme range. `visemeAt(time)` is a deterministic binary search; `GameSession`
attaches the LIP matching a dialogue voice `Sound` resref and exposes
`lipTime`/`lipViseme`.

## 8. Validation

`--audio-test` asserts:

- area music `mus_theme_sith`, ambient `al_en_starforge`, EAX `HALLWAY`;
- 45 sound emitters and a playable UTS object;
- MP3 (`as_el_alert`) and PCM (`gui_click`) decode plus 48 kHz stereo resample;
- music/ambient voices and a non-silent area mix;
- mixer RMS, distance attenuation, stereo pan, EAX wet path, stop-all and loop
  wrap;
- `audio_script_test.ncs` `PlaySound`, `MusicBackgroundChangeDay`,
  `AmbientSoundChangeDay` and `AmbientSoundSetDayVolume`;
- SSF slot/fill counts, `SoundSetFile` -> `soundset.2da` mapping, appearance
  footstep mapping, five creature bark slots and stone/metal/wood footsteps.

`--media-test` asserts all 61 Bink containers/frame tables, movie
play/queue/clock, all 18,206 LIP resources and a viseme sample.

## 9. Limits

Bink pixel decode/presentation and facial morph application are Sprint 14.
Full `.gui`, minigames, localization, mods/Override precedence, KotOR 2 world
loading, faction reputation and the P6 checklist remain future sprints.
