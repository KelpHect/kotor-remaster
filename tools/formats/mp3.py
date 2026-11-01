#!/usr/bin/env python3
"""MP3/BMU container validators.

* :func:`parse_mp3_stream` walks a raw MPEG audio frame stream (used for
  the MP3 data inside streammusic/streamsounds .wav files, whose RIFF data
  chunk or ~470-byte prefix is MP3 -- verified on the shipped loose files).
* :func:`parse_bmu` validates a BMU container ("BMU " prefix followed by a
  u32 size and an MP3 stream).  No .bmu files ship in the reference install,
  so the BMU wrapper layout is **inferred** from docs/recon/02 ("BMU is MP3
  with a BMU prefix"); the MP3 frame walk itself is verified on shipped
  stream data.

MPEG frame header (4 bytes, big-endian bit fields): 11-bit sync 0x7FF,
2-bit version (!= 1 reserved), 2-bit layer (!= 0), 1 protection bit,
4-bit bitrate index (!= 0, != 15), 2-bit sample-rate index (!= 3),
1 padding, ...  Frame size = 144 * bitrate / sample_rate + padding
(MPEG1 Layer III) or 72 * bitrate / sample_rate + padding (Layer I/II
variants handled via the tables below).
Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass


class Mp3Error(ValueError):
    pass

BMU_MAGIC = b"BMU "

_BITRATES = {  # version group -> layer_bits -> [index] kbps
    # layer_bits: 3 = Layer I, 2 = Layer II, 1 = Layer III
    3: {  # MPEG1
        3: [0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0],  # Layer I
        2: [0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0],    # Layer II
        1: [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0],     # Layer III
    },
    2: {  # MPEG2/2.5
        3: [0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0],
        2: [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0],
        1: [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0],
    },
}
_RATES = {
    3: [44100, 48000, 32000],   # MPEG1
    2: [22050, 24000, 16000],   # MPEG2
    0: [11025, 12000, 8000],    # MPEG2.5
}


@dataclass
class Mp3Frame:
    offset: int
    size: int
    version: int
    layer_bits: int
    bitrate: int
    sample_rate: int


@dataclass
class Mp3Stream:
    frames: tuple[Mp3Frame, ...]
    start: int
    end: int  # offset just past the last frame


def _frame_size(version_bits: int, layer_bits: int, br_idx: int, sr_idx: int,
                padding: int) -> tuple[int, int, int]:
    vgroup = 3 if version_bits == 3 else 2  # 3=MPEG1, 2=MPEG2/2.5
    if version_bits == 1:
        raise Mp3Error("reserved MPEG version bits")
    if layer_bits == 0:
        raise Mp3Error("reserved layer")
    table = _BITRATES[vgroup][layer_bits]
    if br_idx in (0, 15):
        raise Mp3Error(f"bad bitrate index {br_idx}")
    if sr_idx == 3:
        raise Mp3Error("bad sample-rate index")
    bitrate = table[br_idx] * 1000
    rate = _RATES[version_bits][sr_idx]
    if layer_bits == 3:  # Layer I
        size = (12 * bitrate // rate + padding) * 4
    else:
        coef = 144 if version_bits == 3 else 72
        size = coef * bitrate // rate + padding
    return size, bitrate, rate


def parse_mp3_stream(data: bytes, start: int = 0, end: int | None = None,
                     name: str = "<memory>", max_frames: int = 1_000_000,
                     strict_end: bool = True) -> Mp3Stream:
    """Walk MPEG frames in data[start:end].

    With ``strict_end=True`` the walk must end exactly at ``end``; with
    ``False`` it stops at the first non-sync byte (used for the MP3 prefix
    of streamsounds .wav files, which ends inside a LAME tag).
    """
    if end is None:
        end = len(data)
    if not 0 <= start <= end <= len(data):
        raise Mp3Error(f"{name}: bad range [{start},{end}) in {len(data)} bytes")
    frames = []
    pos = start
    while pos < end:
        if end - pos < 4:
            if strict_end:
                raise Mp3Error(f"{name}: trailing {end - pos} bytes at {pos} (no frame header)")
            break
        b0, b1, b2, b3 = data[pos], data[pos + 1], data[pos + 2], data[pos + 3]
        if b0 != 0xFF or (b1 & 0xE0) != 0xE0:
            if strict_end:
                raise Mp3Error(f"{name}: lost sync at {pos} (0x{b0:02x} 0x{b1:02x})")
            break
        version_bits = (b1 >> 3) & 0x3
        layer_bits = (b1 >> 1) & 0x3
        padding = (b2 >> 1) & 0x1
        br_idx = (b2 >> 4) & 0xF
        sr_idx = (b2 >> 2) & 0x3
        size, bitrate, rate = _frame_size(version_bits, layer_bits, br_idx, sr_idx, padding)
        if size < 4:
            raise Mp3Error(f"{name}: implausible frame size {size} at {pos}")
        if pos + size > end:
            raise Mp3Error(f"{name}: frame at {pos} size {size} overruns end {end}")
        frames.append(Mp3Frame(pos, size, version_bits, layer_bits, bitrate, rate))
        pos += size
        if len(frames) > max_frames:
            raise Mp3Error(f"{name}: more than {max_frames} frames")
    if not frames:
        raise Mp3Error(f"{name}: no MP3 frames in [{start},{end})")
    return Mp3Stream(tuple(frames), start, pos)


@dataclass
class Bmu:
    declared_size: int
    stream: Mp3Stream
    size: int


def parse_bmu(data: bytes, name: str = "<memory>") -> Bmu:
    """Validate a BMU container: 'BMU ' + u32 total size + MP3 stream.

    The wrapper layout is inferred (no shipped .bmu in this install);
    the MP3 stream itself is strictly walked.
    """
    if len(data) < 8:
        raise Mp3Error(f"{name}: shorter than 8-byte BMU header")
    if data[:4] != BMU_MAGIC:
        raise Mp3Error(f"{name}: bad magic {data[:4]!r} (expected {BMU_MAGIC!r})")
    declared, = struct.unpack_from("<I", data, 4)
    if declared != len(data):
        raise Mp3Error(f"{name}: declared size {declared} != file size {len(data)}")
    stream = parse_mp3_stream(data, 8, len(data), name)
    return Bmu(declared, stream, len(data))


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one MP3/BMU file (read-only)")
    ap.add_argument("path")
    ap.add_argument("--start", type=int, default=0, help="stream start offset")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    try:
        if data[:4] == BMU_MAGIC:
            b = parse_bmu(data, args.path)
            print(f"{args.path}: BMU size={b.declared_size} frames={len(b.stream.frames)}")
        else:
            s = parse_mp3_stream(data, args.start, len(data), args.path)
            print(f"{args.path}: frames={len(s.frames)} first={s.frames[0].bitrate // 1000}"
                  f"kbps {s.frames[0].sample_rate}Hz")
    except Mp3Error as exc:
        print(f"{args.path}: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
