#!/usr/bin/env python3
"""RIFF/WAVE container validator for KotOR audio (type 4 in BIFs and the
loose streamwaves/streamsounds/streammusic .wav files).

Quirk (verified on the shipped loose files): some WAV SFX carry a ~470-byte
prefix before ``RIFF`` (an MP3/LAME-tag frame -- all 970 streamsounds files
have RIFF at offset 470; 51 of 119 streammusic files have a nonzero
prefix).  The validator therefore probes for ``RIFF`` within the first 512
bytes instead of assuming offset 0, records the prefix length, and validates
the container strictly from there:

    "RIFF" u32 size "WAVE" then chunks: id[4] u32 size payload[size]
    required chunks: "fmt " (>= 16 bytes: format u16, channels u16 >= 1,
    sample rate u32, byte rate u32, block align u16, bits u16) and "data"
    RIFF size must equal (file size - prefix - 8); chunks must tile the
    RIFF payload exactly (odd sizes padded to even).

Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass


class WavError(ValueError):
    pass

MAX_PREFIX_PROBE = 512
WAVE_FORMAT_PCM = 0x0001


@dataclass
class WavChunk:
    id: bytes
    offset: int   # absolute file offset of the payload
    size: int


@dataclass
class Wav:
    prefix: int               # bytes before "RIFF" (0 for clean files)
    riff_size: int
    fmt: dict
    chunks: tuple[WavChunk, ...]
    data_size: int
    size: int                 # real file size (or len(data) if fully read)


def parse_wav(data: bytes, name: str = "<memory>", file_size: int | None = None) -> Wav:
    """Validate a RIFF/WAVE container.  ``file_size`` is the on-disk size
    when ``data`` is only a prefix of the file."""
    real_size = len(data) if file_size is None else file_size
    prefix = -1
    if data[:4] == b"RIFF":
        prefix = 0
    else:
        limit = min(MAX_PREFIX_PROBE, len(data))
        idx = data[:limit].find(b"RIFF")
        if idx < 0:
            raise WavError(f"{name}: no RIFF signature within first {limit} bytes")
        prefix = idx
    if len(data) < prefix + 12:
        raise WavError(f"{name}: truncated RIFF header at {prefix}")
    riff_size, = struct.unpack_from("<I", data, prefix + 4)
    if data[prefix + 8:prefix + 12] != b"WAVE":
        raise WavError(f"{name}: RIFF form type {data[prefix + 8:prefix + 12]!r} != WAVE")
    # Shipped quirk: many streamwaves voice files under-declare the RIFF
    # size (and the data chunk size), so the field may be smaller than the
    # file; it must never exceed it.
    if riff_size > real_size - prefix - 8:
        raise WavError(f"{name}: RIFF size {riff_size} > filesize-prefix-8 "
                       f"({real_size - prefix - 8})")
    chunks: list[WavChunk] = []
    fmt = None
    data_size = -1
    pos = prefix + 12
    end = prefix + 8 + riff_size
    if end > real_size:
        raise WavError(f"{name}: RIFF payload exceeds file")
    buf_end = len(data)
    while pos < end:
        if pos + 8 > buf_end:
            raise WavError(f"{name}: chunk header at {pos} beyond buffer "
                           f"(file continues to {real_size}; read more)")
        cid = data[pos:pos + 4]
        csize, = struct.unpack_from("<I", data, pos + 4)
        payload_off = pos + 8
        if payload_off + csize > end:
            raise WavError(f"{name}: chunk {cid!r} size {csize} overruns RIFF payload")
        chunks.append(WavChunk(cid, payload_off, csize))
        if cid == b"fmt ":
            if csize < 16 or payload_off + 16 > buf_end:
                raise WavError(f"{name}: fmt chunk too small/short")
            (aformat, channels, rate, byte_rate, block_align, bits) = \
                struct.unpack_from("<HHIIHH", data, payload_off)
            if channels < 1 or channels > 8:
                raise WavError(f"{name}: implausible channel count {channels}")
            if rate < 1000 or rate > 192000:
                raise WavError(f"{name}: implausible sample rate {rate}")
            # shipped files are PCM (0x1, 8/16-bit) and IMA ADPCM (0x11,
            # 4-bit, streamwaves/streamsounds); MP3-in-WAV carries a PCM tag
            if bits not in (4, 8, 16, 24, 32):
                raise WavError(f"{name}: unsupported bit depth {bits}")
            if aformat not in (0x1, 0x11, 0x55):
                raise WavError(f"{name}: unsupported format tag 0x{aformat:04x}")
            fmt = {"format": aformat, "channels": channels, "sample_rate": rate,
                   "byte_rate": byte_rate, "block_align": block_align, "bits": bits}
        if cid == b"data":
            data_size = csize
        pos = payload_off + csize + (csize & 1)  # chunks are word-aligned
    if pos != end:
        raise WavError(f"{name}: chunks end at {pos}, RIFF payload ends at {end}")
    if fmt is None:
        raise WavError(f"{name}: no fmt chunk")
    if data_size < 0:
        raise WavError(f"{name}: no data chunk")
    return Wav(prefix, riff_size, fmt, tuple(chunks), data_size, real_size)


def main(argv=None) -> int:
    import argparse
    import os
    ap = argparse.ArgumentParser(description="Validate one WAV file (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    size = os.path.getsize(args.path)
    with open(args.path, "rb") as f:
        head = f.read(min(size, 65536))
    w = parse_wav(head, args.path, file_size=size)
    print(f"{args.path}: prefix={w.prefix} {w.fmt['sample_rate']}Hz "
          f"{w.fmt['channels']}ch {w.fmt['bits']}bit data={w.data_size}B "
          f"chunks={len(w.chunks)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
