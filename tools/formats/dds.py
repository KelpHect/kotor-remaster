#!/usr/bin/env python3
"""DDS container-header validator (DirectDraw Surface).

No DDS files ship in the reference install (verified by extension/magic
scan; see docs/recon/18_FORMAT_MATRIX.md), so this validator is exercised
with synthetic fixtures.  Layout per the DDS spec:

    0x00  char[4] "DDS "
    0x04  u32 header size == 124
    0x08  u32 flags
    0x0C  u32 height, 0x10 u32 width (both >= 1)
    0x14  u32 pitch/linear size
    0x18  u32 depth
    0x1C  u32 mipmap count
    0x48  pixel format: u32 size == 32, u32 flags, char[4] fourCC,
          u32 rgb bit count, 4 x u32 bit masks
    0x74  u32 caps, 0x78 u32 caps2..4, 0x84 reserved...

Validation: magic, size field, dimensions, pf size, known fourCC set
(DXT1/DXT3/DXT5/DXN/ATI1/ATI2/RGBG/GRBG/..., or 0 with RGB flags), and
optional exact payload size for uncompressed formats.
Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass


class DdsError(ValueError):
    pass

MAGIC = b"DDS "
KNOWN_FOURCC = {b"DXT1", b"DXT2", b"DXT3", b"DXT4", b"DXT5", b"DXN ",
                b"ATI1", b"ATI2", b"BC4U", b"BC5U", b"RGBG", b"GRBG",
                b"UYVY", b"YUY2", b"DX10"}
FOURCC_BLOCK_BYTES = {b"DXT1": 8, b"DXT2": 16, b"DXT3": 16, b"DXT4": 16,
                      b"DXT5": 16, b"DXN ": 16, b"ATI1": 8, b"ATI2": 16,
                      b"BC4U": 8, b"BC5U": 16}


@dataclass
class Dds:
    flags: int
    width: int
    height: int
    pitch: int
    depth: int
    mipmaps: int
    pf_flags: int
    fourcc: bytes
    rgb_bits: int
    caps: int
    size: int


def parse_dds(data: bytes, name: str = "<memory>") -> Dds:
    if len(data) < 128:
        raise DdsError(f"{name}: shorter than 128-byte DDS header")
    if data[:4] != MAGIC:
        raise DdsError(f"{name}: bad magic {data[:4]!r}")
    hsize, flags, height, width, pitch, depth, mips = struct.unpack_from("<7I", data, 4)
    if hsize != 124:
        raise DdsError(f"{name}: header size field {hsize} != 124")
    if width < 1 or height < 1 or width > 65535 or height > 65535:
        raise DdsError(f"{name}: implausible dimensions {width}x{height}")
    pf_size, pf_flags, fourcc, rgb_bits = struct.unpack_from("<II4sI", data, 0x4C)
    if pf_size != 32:
        raise DdsError(f"{name}: pixel format size {pf_size} != 32")
    if fourcc == b"\x00\x00\x00\x00":
        if not pf_flags & 0x40:  # DDPF_RGB
            raise DdsError(f"{name}: fourCC 0 without DDPF_RGB flag")
        if rgb_bits not in (8, 16, 24, 32):
            raise DdsError(f"{name}: unsupported RGB bit count {rgb_bits}")
    elif fourcc not in KNOWN_FOURCC:
        raise DdsError(f"{name}: unknown fourCC {fourcc!r}")
    caps, = struct.unpack_from("<I", data, 0x74)
    if not caps & 0x1000:  # DDSCAPS_TEXTURE
        raise DdsError(f"{name}: missing DDSCAPS_TEXTURE (caps 0x{caps:08x})")
    if fourcc == b"DX10":
        raise DdsError(f"{name}: DX10 extended header not supported by this validator")
    # optional exact-size check for block formats with full mip chains
    if fourcc in FOURCC_BLOCK_BYTES and mips:
        block = FOURCC_BLOCK_BYTES[fourcc]
        total = 0
        w, h = width, height
        for _ in range(mips):
            total += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block
            w = max(1, w // 2)
            h = max(1, h // 2)
        if 128 + total != len(data):
            raise DdsError(f"{name}: payload {len(data) - 128} != expected {total}")
    return Dds(flags, width, height, pitch, depth, mips, pf_flags, fourcc, rgb_bits,
               caps, len(data))


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one DDS file (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    d = parse_dds(data, args.path)
    print(f"{args.path}: {d.width}x{d.height} fourcc={d.fourcc!r} mips={d.mipmaps}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
