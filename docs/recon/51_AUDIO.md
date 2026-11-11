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
- `AmbientSndDayVol`, `AmbientSndNitVol` - 0..127 (retail clamps to 0x7f at 005d5b50; shipped
  values exceed 100, so it is not a percentage);
- `EnvAudio` - row id in `soundeax.2da`;
- `MusicDelay` - milliseconds.

`ambientmusic.resource` is a `streammusic` resref. `ambientsound.resource` is
an ambient loop resref. `soundeax.label` selects one of 24 presets from
`GENERIC` through `UNDERWATER`/`NOREVERB`.

`GIT SoundList` stores `TemplateResRef` plus position. The UTS template
(type 2035) supplies `Sounds[].Sound`, `Active`, `Continuous`, `Looping`,
`Positional`, `Random`, `Elevation`, `MinDistance`, `MaxDistance`, `Volume`,
`Interval`, `IntervalVrtn`, `VolumeVrtn`, `PitchVariation`, `Times`, `Hours`,
`FixedVariance`, `RandomPosition`, `RandomRangeX/Y`. `Volume` is 0..127 (20 of the 45
emitters of `end_m01aa` exceed 100); the loader 005c9040 does not read `Elevation` or
`Priority`.

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

## 4. Environment reverb (parameter-faithful, not bit-exact Miles/EAX)

Retail drives Creative EAX through the Miles Sound System provider
(`Msseax.m3d`). Evidence from `swkotor.exe` 1.0.3.0:

- `005d66d0` sets the room type (`AIL_set_3D_room_type`,
  `AIL_set_digital_master_room_type`) and forwards the EAX2/EAX3 preference keys
  (size, diffusion, room, room HF, decay time, decay HF ratio, reflections,
  reflections delay, reverb, reverb delay, echo, modulation, air absorption, HF
  reference) from a 24-row table at VA `0x74c610` (0x70 bytes per row). The table
  matches the published EAX 3.0 presets; row 23 (`NOREVERB`) holds the GENERIC
  parameters, so it is not silent by itself. Ids >= 24 only set room type 0.
- The environment id is the `soundeax.2da` row index (the 2DA has only `label`
  and a description). It comes from the ARE `Rooms` list (`EnvAudio` per room,
  loader `00509780`); the GIT `AreaProperties/EnvAudio` is not read. 62 of the
  106 shipped areas mix several environments. `005ee860` applies the room's
  `EnvAudio` (and `AmbientScale`) when the room under the party changes.
- Only 3D samples (`AIL_set_3D_sample_effects_level`, `005d71c0`) and positional
  streams (`AIL_set_stream_reverb_levels`, `005dc560`) get a reverb send, a single
  global level that defaults to 0.6 (`005d8813`). Music and movie streams have no
  reverb call. `AmbientScale` is a separate per-room volume multiplier ramped at
  1.0 per second (not a reverb level) and is not implemented here.

The engine renders those parameters with `audio/Reverb.cpp`: a mono send into
an 8-line feedback delay network (Householder mixing, Jot absorption gains for
`DecayTime` and `DecayHFRatio`) plus tapped early reflections at
`ReflectionsDelay`, late onset at `ReflectionsDelay + ReverbDelay`, room/
reflections/reverb levels in millibel, `RoomHF` low-pass, input diffusion and an
echo comb. The mixer holds two instances and equal-power crossfades (150 ms) on a
room change. Everything is preallocated, runs with flush-to-zero set, and is
deterministic. Not reproduced: the Creative DSP's late-field texture, modulation
(`UNDERWATER`), air absorption, LF decay and room rolloff, and `DecayHFRatio`
above 1 (`CAVE`, `PARKINGLOT`).

`--audio-test` (`kotor_audio`) measures this against the preset parameters with
an impulse: Schroeder-integral RT60, early-reflection onset, HF/LF decay ratio,
L/R correlation, denormal cost, determinism, crossfade continuity and CPU cost.

## 5. Integration

`GameSession` owns `AudioSystem`. Area load resolves music/ambient, sets the
reverb preset and starts loops. `update` moves the listener with the player.
Dialogue entries/replies call `playVoice` for non-empty `Sound` fields.
Script actions map to the audio API; `SoundObject*` actions resolve the script
object id back to the GIT SoundList entity.

### 5.1 Area ambience runtime (game/Ambience.cpp, game/ScriptAmbience.cpp)

`AreaAmbience` (owned by `AudioSystem`) runs the music director, the ambient loop and
the GIT sound objects; `--family-test ambience` (`kotor_family_ambience`) measures it on
the Mixer. Retail evidence is cited by address in the source header; inferred rules are
labelled there (fade-in length, MusicDelay replay rule, RandomPosition box, 3D voice
stealing, the 8 s combat state standing in for the battle timer).

- Sound objects start at area load (once the first listener position is known) and obey
  Active, Continuous, Looping, Random, Interval +- IntervalVrtn, VolumeVrtn, PitchVariation,
  FixedVariance (0.5..2.0 only), RandomPosition, Times/Hours; 16 positional voices.
- Music: day/night rows with a 1 s crossfade, battle row + random stinger on a combat
  rising edge, MusicDelay, conversation and UI layers. The main-menu theme is named by
  the executable (005f9af0), not by `ambientmusic` row 1 (`mus_gui_start` has no file).
- Ambient: volume/127 times the room `AmbientScale` (ramped at 1.0/s).
- Mixer: bus pause flag (`setMovieMode`, retail pauses and resumes, 005d82c0 / 005d83f0), per-bus peak/RMS meters, per-voice fades.
- Bus defaults come from `swkotor.ini` [Sound Options] (85/85/85/100).
- Nothing has been listened to; the gate asserts mixer-level quantities only.

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

> Superseded by section 6.1 (retail 0061a2d0).

### 6.1 Priority groups and the retail sound rules (swkotor.exe 1.0.3.0)

The executable never reads `soundgain.2da` (the string is not in it). Every sound is started with
a **priority group** (`PriorityGroups.2da`, 27 rows, loaded by 005d7bd0; blank cells keep the
005d6c10 defaults: priority 0, volume 127, max playing 255, interrupt 1, distances 20 / 10,
variance 0). The group decides level, user-volume category, concurrency and rate variance:

- Category (005d6a90 / 005d6ad0): groups 8, 9, 16, 17, 26 use the dialog volume, 1 and 2 the music
  volume, everything else the sound-effects volume.
- Level: a sample is `floor(group * source / 127) / 127` times the category setting (005db800); a 3D
  sample is `pow(level * setting, 0.6)`; a stream is `group/127 * source/127 * setting` (005dc930).
- Cap (005db4d0): a full group either interrupts one of its sounds (Interrupt) or drops the new one.
- Rate (005db3d0): PlaybackVariance moves the rate by a random amount, one time in two up by up to the
  fraction, otherwise down by up to half of it; FixedVariance (005db980) accepts 0.5 .. 2.0 only.
- Users: music 2 (menu 005f9af0, load 005f6480, area 0068e1f0, battle 0068e610), stinger 1 (0068db30),
  conversation AmbientTrack and ambient loop 4, sound objects 4 / 5 (loops) and 21 / 22 (one shots,
  0066b420), room sounds 25, bark bubbles 26, GUI 11, dialogue VO 9, hit and swing 15, spells 13,
  doors and placeables 22, ground hit 0 (0060b400), minigame 24.
- The table's distance columns cannot be told apart in code (Miles does the falloff): the engine treats
  the nearer value as full level and the farther as silent. That one point is inferred.
  Streams are not Miles-driven: a stream's own code (Update3DStream 005dc560, called from SetVolume and every
  Render) sets its distances from the group's columns (SetPriorityGroup 005dcd50: +0x48 = MaxVolumeDist column,
  +0x4c = MinVolumeDist column, a sound object overrides them with its UTS MinDistance / MaxDistance in 0066b420)
  and computes `volume = pow(level * setting * f, 0.6)` with `f = (max - d) / (max - min)` for d > min (floored at
  0, d > max stops the stream, Render), times `1 + 0.125 * dot` when the source is behind the listener
  (dot = direction . listener forward, 005dbbb0), and a pan of 0.5 * |dot| or 1 - 0.5 * |dot| clamped to
  0.05 .. 0.95 (pan not done). The remaining factor is the obstruction: OcclusionRender (005d88f0) writes
  `[stream + 0x80] = GetObstructionFactor (0050ccf0)` and the gain is multiplied by `1 - [+0x80]`. The factor walks the
  line source -> listener with CSWSArea::ClearLineOfSight (0050c330), restarting 0.1 m past each hit; every geometry
  hit adds 0.4 (objects 0.4 and / or 0.55), a sum above 1 gives 1, and the result is cached by
  GetClosestRecentSound (005d6ef0: 0.5 m, 175 ms). Ours: Audio.cpp voiceGain (stream3d voices, looping positional
  sound objects and positional group streams) and GameSession::soundObstruction (area geometry with blocksSight only;
  doors and objects are not counted yet). The triangle test (004ab9d0) is one sided: the segment must start on the
  normal's side of the plane (normal (p1 - p0) x (p2 - p1)) and end on the other, so only front faces count; the
  segment runs from the source to the listener (005d88f0 calls 0050ccf0 with the source first). Measured against the
  retail stream dump of danm13 (SC/audio/retail/r5_danm13_sounds.tsv) at the retail listener 187.985 74.339 14.412:
  obstruction 0.4 / 0.8 / 0.8 and finals 0.195 / 0.094 / 0.090, the same as ours. 2D streams pass the plain level
  (AIL_set_stream_volume_levels, level * bus setting); 3D streams pass pow(level * setting * f * (1 - obstruction), 0.6)
  with AIL_set_stream_volume_pan. At those identical values our rendered steam hiss is about 7 dB louder than the
  loopback, so Miles' volume_pan taper is still unknown (RETAIL_REQUESTS.tsv line 43). One-shot 3D samples (Miles)
  keep the squared ramp.
- Visual effect cessation sound: retail asks the row for "SoundCessation" (006a6590, 00690510); the shipped
  column is "soundcessastion", so the lookup fails, the resref is blank and a cessation plays nothing.

Footsteps (CSWCCreature::PlayFootstep 0061a2d0, from the snd_Footstep clip event): the row is the
appearance `footsteptype` as is (blank is silent, rows 5..10 are real rows; stealth forces row 0), a
filled `Rolling` cell (rows 8, 9) means no step sound (a looped rolling sound plays instead, 006105b0,
not done), the column is surfacemat.2da `Name` plus rand() % 3 (blank Name, as Swamp, is silent;
NotDefined is dirt; Mud is puddles). PitchOffset replaces the group's variance. Group: controlled
creature 19, SizeCategory 1..3 group 20, else 18. A step further than MaxVolumeDist + 2 m from the
listener is skipped.

Creature voices (PlaySoundSetSound 00611470): enum = slot + 1; a dead creature only plays slot 15; the
controlled creature speaks in group 8, others 17 (SizeCategory < 4) or 16; one voice per creature.
Server rules: BroadcastVoiceChat (004f2520) reaches creatures within 30 m of the player's creature;
battle cry on entering combat 30 percent, slot rand % 5 (004f2610, 004ec8e0); attack grunt slots 9..11
(AddFireForgetAnimation 006183d0) and pain slots 12, 13 (HitEvent 00617eb0) always for the controlled
creature and one time in five for the others; death 15 (004e0ac0), critical hit 16 (005bb440),
poison 27 (004db320).

Movies (ACT_733, WaitForMovie, PlayLoadModuleMovies): sound mode 3 pauses every stream and sample
(005d82c0) and the mode pop resumes them (005d83f0). Music (0068e1f0): FadeAndStop 800 ms when the
area track stops, 1000 ms on a layer switch, retry after 10 s on a failed play.

Lip sync (00616310): the LIP shape is an unsigned byte, the talk-clip time is (shape + 1) / 16 of the
clip, key times are divided by the LIP length and a last key within 1e-5 of 1.0 is snapped to 1.0.

Rolling and floating droids (footstepsounds rows 8, 9): CSWCCreature 006105b0 starts the `Rolling` cell as a
looped stream in the footstep group, 00610840 follows the animation id (0x2712 + animations.2da row):
walk ids 0x2712, 0x2713, 0x276d, 0x2795 play at rate 1.0, run ids 0x2714, 0x276e at PitchOffset + 1.0, every
other animation stops it. There is no footstep timer for the player: steps come only from snd_Footstep.

Player sound set: SetSoundSet (0060b7c0) is called from the chargen class-select creatures (006dc9cf) with
the byte of the table at 007a2684 (soundset.2da row 85 for a male, 83 for a female) and from the creature
update message (00656217); stats +0x9d8 is otherwise only read from a GFF, so the player keeps that one.

Listener (005f5370): the camera position moved 0.75 (007a22a0, clamped 0..1) of the way to the followed
creature, with the camera's orientation.

Battle music (SetExcitedState 004f3580, UpdateExcitedStateTimer 004ed110, ExcitedDuration.2da rows 1 Damage
and 2 SpellCast, 10000 ms): a creature that enters combat gets the timer; if it is longer than the time left
and an enemy is within 30 m the area plays its battle track, and when the timer runs out the track stops.

Fades: ambient loop FadeAndStop 6000 ms (0068ebf7), fade in PriorityGroups FadeTime 1000 (rows 2, 4, 5, 25);
music FadeAndStop 800 ms stopping the area track and 1000 ms switching layers; a failed music play is tried
again after 10 s (0068e1f0 stores 0x2710 in +0xd0). Over the 3D voice budget 005d8eb0 steals the playing
sound with the largest group Priority number, only when it is greater than the new sound's.

Hit material (HitEvent 00617470): the appearancesndset armortype cell, else the armortype of the base item
worn in the body slot, else "leather". Critical hit: ResolveDamage 005bb440 sends enum 0x11 from the attacker
when the attack result is 2. Poison 0x1c (004db625), stealth 0x15 (00613930).

Not done: the SpellCast excited row (2), the exact re-arm events of the excited timer (the combat-state refresh
stands in), the dialogue-camera listener branch (0x1070 adds the creature's height).

## 7. Bink and LIP media

Bink 1 header (all 61 shipped movies, version `i`):

```text
0x00 "BIK" + version
0x04 filesize-8
0x08 frame count, 0x0C largest frame, 0x10 duplicate frame count
0x14 width, 0x18 height
0x1C u32 fps numerator, 0x20 u32 fps denominator (2997/100 = 29.97)
0x38 (frames+1) u32 frame offsets, EOF sentinel == filesize
```

The engine validates every header and frame table, stores the playback clock
and implements `PlayMovie`/`QueueMovie`/`PlayMovieQueue`/`IsMoviePlaying`.
Dimensions are 640x272 (53), 640x360 (5), 640x480 (3). `BinkPlayer` links
FFmpeg 9.0.2 `avcodec`/`avformat`/`swscale`, decodes Bink 1 frames to RGBA and
presents them through a GL/Vulkan ImGui texture at the header clock. `01a.bik`
frame 0 is 640x272 RGBA (696,320 bytes), SHA-256
`301f27f0e6022367e847de4861de2be72c25c1ffd0ccfdadbd920673f9bf1430`,
matching the recorded FFmpeg oracle. Bink Audio (DCT) is decoded at 44.1 kHz
stereo to interleaved float with the same FFmpeg decoder; its first-second
PCM SHA-256 matches the oracle and the movie bus cursor is locked to the
video clock (`docs/recon/55_BINK_AUDIO.md`).

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

`--media-test` asserts all 61 Bink containers/frame tables, native pixel
decode of `01a.bik` frame 0 against the recorded SHA-256, frame 1, rewind and
replay determinism, Bink Audio decode against the PCM oracle plus movie-bus
cursor sync/seek/stop, the playback frame/clock, movie play/queue, all 18,206
LIP resources and a viseme sample.

## 9. Next sprint

The P6 content checklist is planned in `docs/SPRINT14.md`. The shipped-data boundary is
recorded here: 107 heads carry 0 viseme clips, so facial deformation needs an
engine rig rather than a data-only mapping.
