# Recon 55 - Bink Audio decode and movie-clock sync

Status: Sprint 13 implementation note. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Shipped stream

`01a.bik` audio: `binkaudio_dct`, `fltp`, **44,100 Hz**, **2 channels**,
120.32 s (5,306,127 frames). All shipped movies carry a Bink Audio track.

FFmpeg oracle for the first second:

```text
ffmpeg -v error -i 01a.bik -map 0:a:0 -t 1 -f f32le -acodec pcm_f32le -
bytes: 352800 (44100 * 2 * 4)
sha256: 8a6f3225ace005d76c131a31479ecfc48bb548a3ed6fcfb0f297cc50d9c96622
```

## Decoder

`BinkPlayer::decodeAudio` opens a second `AVFormatContext` for the movie,
selects the best audio stream, opens the FFmpeg Bink Audio decoder and appends
every received frame as interleaved float:

- `FLTP`/`FLT`, `S16P`/`S16`, `S32P`/`S32` are accepted;
- frame PTS is rescaled to samples (44.1 kHz time base) and gaps are filled
  with silence, so the track stays on the video clock;
- the track is trimmed to the video duration;
- the result is exposed as `BinkAudioTrack { sampleRate, channels, samples }`.

The engine's first second hashes to the same
`8a6f3225...c96622` as the FFmpeg PCM oracle.

## Mixer / sync

A fifth mixer bus (`Bus::Movie`) carries the movie track. `GameSession`:

1. resamples the native 44.1 kHz track to 48 kHz stereo;
2. plays it on the movie bus without looping;
3. sets the voice cursor to `movieTime_` every update, so dropped/duplicated
   video frames do not drift the audio;
4. seeks the cursor with `seekMovie`, and stops the voice on movie end,
   queue change or `stopMovie`.

`Mixer::setVoiceCursor`/`voiceCursor` expose the frame cursor for deterministic
assertions.

## Gate

`--media-test` asserts:

- `bink_audio` presence, 44.1 kHz / 2 channels, frame count, duration within
  0.5 s of the video clock;
- first-second PCM SHA-256 equals the FFmpeg oracle;
- movie audio voice starts at 0, follows `movieTime`, seeks to 5.0 s, survives
  a queue change and stops with the movie.

Evidence: `docs/evidence/s13/media.log`; oracle note
`docs/evidence/s13/bik-audio.txt`.
