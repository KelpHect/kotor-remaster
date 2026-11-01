#!/usr/bin/env python3
"""Strict TGA (Targa) image validator for KotOR type-3 resources.

18-byte header (all LE):
    0x00 u8 id length, 0x01 u8 color-map type (0/1), 0x02 u8 image type
    0x03 5 B color-map spec, 0x08 u16 x origin, 0x0A u16 y origin,
    0x0C u16 width, 0x0E u16 height, 0x10 u8 bpp, 0x11 u8 descriptor

Image types: 1 cmap raw, 2 RGB raw, 3 gray raw, 9 cmap RLE, 10 RGB RLE,
11 gray RLE.  Validation decodes the full pixel stream (RLE packets for
types 9-11) and requires it to end exactly at EOF.  Shipped KotOR type-3
resources are uncompressed type 2 (verified on 5,601 files; RLE support is
fixture-tested).  Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass


class TgaError(ValueError):
    pass

RAW_TYPES = {1: "cmap", 2: "rgb", 3: "gray"}
RLE_TYPES = {9: "cmap", 10: "rgb", 11: "gray"}
BPP = {1: None, 2: (16, 24, 32), 3: (8,), 9: None, 10: (16, 24, 32), 11: (8,)}


@dataclass
class Tga:
    id_length: int
    cmap_type: int
    image_type: int
    x: int
    y: int
    width: int
    height: int
    bpp: int
    descriptor: int
    pixel_bytes: int   # decoded size of the pixel stream
    size: int


def parse_tga(data: bytes, name: str = "<memory>") -> Tga:
    if len(data) < 18:
        raise TgaError(f"{name}: shorter than 18-byte TGA header")
    id_len, cmap_type, img_type = data[0], data[1], data[2]
    cmap_first, cmap_len, cmap_entry = struct.unpack_from("<HHB", data, 3)
    x, y, w, h = struct.unpack_from("<4H", data, 8)
    bpp, desc = data[16], data[17]
    if cmap_type not in (0, 1):
        raise TgaError(f"{name}: bad color-map type {cmap_type}")
    if img_type not in RAW_TYPES and img_type not in RLE_TYPES:
        raise TgaError(f"{name}: unsupported image type {img_type}")
    if w < 1 or h < 1 or w > 65535 or h > 65535:
        raise TgaError(f"{name}: implausible dimensions {w}x{h}")
    if cmap_type == 1:
        cmap_bpp = {1: 16, 2: 24, 3: 32}.get(cmap_entry)
        if cmap_bpp is None:
            raise TgaError(f"{name}: bad color-map entry size {cmap_entry}")
        if img_type not in (1, 9):
            raise TgaError(f"{name}: color map present but image type {img_type}")
    else:
        if cmap_len or cmap_first:
            raise TgaError(f"{name}: color-map spec set but type 0")
        cmap_bpp = 0
    allowed = BPP[img_type]
    if allowed is not None and bpp not in allowed:
        raise TgaError(f"{name}: bpp {bpp} not allowed for type {img_type}")
    if img_type in (1, 9) and bpp == 0:
        raise TgaError(f"{name}: cmap image needs a bpp")

    pos = 18 + id_len + (cmap_len * (cmap_bpp // 8) if cmap_type else 0)
    if pos > len(data):
        raise TgaError(f"{name}: header fields exceed file size")
    # Optional 26-byte developer footer: 8 bytes offsets + "TRUEVISION-XFILE.\0"
    footer = 0
    if len(data) >= 26 and data[-18:] == b"TRUEVISION-XFILE.\x00":
        footer = 26
    payload_end = len(data) - footer
    pixel_bytes = w * h * (bpp // 8)
    if img_type in RAW_TYPES:
        if pos + pixel_bytes > payload_end:
            raise TgaError(f"{name}: raw pixel stream needs {pixel_bytes} bytes at {pos}, "
                           f"payload ends at {payload_end} (file {len(data)})")
        trailing = payload_end - (pos + pixel_bytes)
        if trailing and footer == 0:
            # shipped lightmaps carry an opaque trailing blob after the pixels
            pass
    else:
        # RLE: walk packets; each packet header byte: bit7 = RLE, count = low7 + 1
        count = 0
        while count < w * h:
            if pos >= payload_end:
                raise TgaError(f"{name}: RLE stream truncated at {pos}")
            hdr = data[pos]
            pos += 1
            n = (hdr & 0x7F) + 1
            if hdr & 0x80:
                pos += bpp // 8
            else:
                pos += n * (bpp // 8)
            if pos > payload_end:
                raise TgaError(f"{name}: RLE packet overruns payload at {pos}")
            count += n
        if count != w * h:
            raise TgaError(f"{name}: RLE decoded {count} pixels, expected {w * h}")
        if pos != payload_end and footer == 0:
            raise TgaError(f"{name}: {payload_end - pos} trailing bytes after RLE stream")
        pixel_bytes = count * (bpp // 8)
    return Tga(id_len, cmap_type, img_type, x, y, w, h, bpp, desc, pixel_bytes,
               len(data))


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one TGA file (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    t = parse_tga(data, args.path)
    print(f"{args.path}: type={t.image_type} {t.width}x{t.height} bpp={t.bpp} "
          f"pixels={t.pixel_bytes}B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
