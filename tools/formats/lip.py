#!/usr/bin/env python3
"""Strict parser for KotOR LIP (lip sync) files (type 3004, inside lips/*.mod).

Layout (verified on the shipped lips/*.mod payloads):

    0x00  char[8] "LIP V1.0"
    0x08  f32     duration (seconds)
    0x0C  u32     frame count
    0x10  frames: count x (f32 time, u8 viseme 0-15), packed 5 bytes each

Validation: exact size (16 + 5*count == file size), times non-decreasing
and within [0, duration], visemes 0-15.  Shipped files start at time 0.0
and end at time == duration (checked as a strict rule; verified on the
corpus).  Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass


class LipError(ValueError):
    pass

MAGIC = b"LIP V1.0"
MAX_VISEME = 15


@dataclass(frozen=True)
class LipFrame:
    time: float
    viseme: int


@dataclass
class Lip:
    duration: float
    frames: tuple[LipFrame, ...]
    size: int


def parse_lip(data: bytes, name: str = "<memory>") -> Lip:
    if len(data) < 16:
        raise LipError(f"{name}: shorter than 16-byte LIP header")
    if data[:8] != MAGIC:
        raise LipError(f"{name}: bad magic {data[:8]!r}")
    duration, count = struct.unpack_from("<fI", data, 8)
    if len(data) != 16 + count * 5:
        raise LipError(f"{name}: size {len(data)} != 16 + 5*{count}")
    frames = []
    prev = -1.0
    for i in range(count):
        t, v = struct.unpack_from("<fB", data, 16 + i * 5)
        if v > MAX_VISEME:
            raise LipError(f"{name}: frame {i} viseme {v} > {MAX_VISEME}")
        if t < prev:
            raise LipError(f"{name}: frame {i} time {t} < previous {prev}")
        if t < -1e-6 or t > duration + 1e-6:
            raise LipError(f"{name}: frame {i} time {t} outside [0, {duration}]")
        prev = t
        frames.append(LipFrame(t, v))
    if count:
        if frames[0].time != 0.0:
            raise LipError(f"{name}: first frame time {frames[0].time} != 0.0")
        if abs(frames[-1].time - duration) > 1e-6:
            raise LipError(f"{name}: last frame time {frames[-1].time} != duration {duration}")
    elif duration != 0.0:
        raise LipError(f"{name}: zero frames but duration {duration}")
    return Lip(duration, tuple(frames), len(data))


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one LIP resource (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    lip = parse_lip(data, args.path)
    print(f"{args.path}: duration={lip.duration:.6f} frames={len(lip.frames)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
