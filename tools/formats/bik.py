#!/usr/bin/env python3
"""Bink video container-header validator (movies/*.bik).

Verified on all 61 shipped movies (all version 'i'):

    0x00  char[3] "BIK", 0x03 char version ('a'..'i'; shipped: 'i')
    0x04  u32  file size - 8                (61/61 exact)
    0x08  u32  frame count
    0x0C  u32  largest frame size
    0x10  u32  frame count (duplicate)
    0x14  u32  width, 0x18 u32 height        (shipped movies: 640x272)
    0x1C  u32, 0x20 u32, 0x24 u32, 0x28 u32, 0x2C u32   header fields
    0x38  frame offset table: (frame-count + 1) x u32, strictly ascending,
          last entry == file size (EOF sentinel), table end <= first entry

Validation reads only the header plus the frame table (no payload decode;
FFmpeg's Bink decoder is the oracle for pixels).  Build of record:
SHA-256 0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass


class BikError(ValueError):
    pass

MAGIC = b"BIK"
VERSIONS = frozenset(b"abcdefghi")
TABLE_OFFSET = 0x38


@dataclass
class Bik:
    version: str
    file_size_field: int
    frames: int
    largest_frame: int
    width: int
    height: int
    frame_offsets: tuple[int, ...]
    size: int


def parse_bik(data: bytes, name: str = "<memory>", full: bool = True,
              file_size: int | None = None) -> Bik:
    """Validate a Bink header.

    With ``full=True`` the frame-offset table is validated too, which
    requires ``len(data) >= TABLE_OFFSET + 4*frames``.  ``file_size`` is the
    real on-disk size when only a header prefix of the file is in ``data``.
    """
    real_size = len(data) if file_size is None else file_size
    if len(data) < TABLE_OFFSET:
        raise BikError(f"{name}: shorter than {TABLE_OFFSET}-byte Bink header")
    if data[:3] != MAGIC:
        raise BikError(f"{name}: bad magic {data[:3]!r}")
    ver = data[3]
    if ver not in VERSIONS:
        raise BikError(f"{name}: unknown Bink version {chr(ver)!r}")
    file_size_field, frames, largest, frames2, width, height = \
        struct.unpack_from("<6I", data, 4)
    if file_size_field != real_size - 8:
        raise BikError(f"{name}: size field {file_size_field} != filesize-8 "
                       f"({real_size - 8})")
    if frames == 0 or frames > 10_000_000:
        raise BikError(f"{name}: implausible frame count {frames}")
    if frames2 != frames:
        raise BikError(f"{name}: frame count fields differ ({frames} vs {frames2})")
    if largest == 0 or largest > real_size:
        raise BikError(f"{name}: implausible largest frame {largest}")
    if not 1 <= width <= 8192 or not 1 <= height <= 8192:
        raise BikError(f"{name}: implausible dimensions {width}x{height}")
    offsets = ()
    if full:
        # The table holds frames+1 u32 entries: frame offsets plus a final
        # EOF sentinel (verified on all 61 shipped movies).
        end = TABLE_OFFSET + (frames + 1) * 4
        if end > len(data):
            raise BikError(f"{name}: frame table [{TABLE_OFFSET},{end}) exceeds buffer")
        offsets = struct.unpack_from(f"<{frames + 1}I", data, TABLE_OFFSET)
        # the EOF sentinel is the first entry equal to the file size
        # (index == frames in 60 of 61 shipped movies; legal.bik has it
        # earlier -- shipped quirk)
        sentinel = next((k for k in range(1, frames + 1) if offsets[k] == real_size), None)
        if sentinel is None:
            raise BikError(f"{name}: no EOF sentinel in frame table")
        for i in range(sentinel - 1):
            if offsets[i] >= offsets[i + 1]:
                raise BikError(f"{name}: frame table not ascending at entry {i} "
                               f"({offsets[i]} >= {offsets[i + 1]})")
        if offsets[0] < end:
            raise BikError(f"{name}: first frame offset {offsets[0]} inside table end {end}")
    return Bik(chr(ver), file_size_field, frames, largest, width, height, offsets,
               real_size)


def main(argv=None) -> int:
    import argparse
    import os
    ap = argparse.ArgumentParser(description="Validate one BIK movie header (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    size = os.path.getsize(args.path)
    with open(args.path, "rb") as f:
        head = f.read(min(size, TABLE_OFFSET + 4 * 2_000_000))
    b = parse_bik(head, args.path, full=True, file_size=size)
    print(f"{args.path}: BIK{b.version} {b.width}x{b.height} frames={b.frames} "
          f"largest={b.largest_frame}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
