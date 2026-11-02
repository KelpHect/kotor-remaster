#!/usr/bin/env python3
"""Clean-room save-thumbnail shim for the Mesa faulting resampler.

The shipped screenshot path is ``FUN_00420f20`` -> ``glReadPixels`` ->
``FUN_0045dad0(src, sw, sh, ch, dw, dh)``.  ``FUN_0045dad0`` faults at
``0x45db21`` under Mesa when the readback data is not what it expects.  The
Frida harness replaces ``FUN_00420f20`` with a synthetic TGA writer and also
replaces ``FUN_0045dad0`` with the same nearest-neighbour resampler implemented
here, so the faulting code is never entered.

This module is the deterministic, testable model of that shim.  It is also a
standalone tool: ``python tools/harness/thumb_shim.py --out Screen.tga`` writes
the same 256x256 24-bpp TGA the hook writes.  No game data is read or written.

Build under test: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path


class ThumbShimError(ValueError):
    pass


def resample_nearest(src: bytes, src_w: int, src_h: int, channels: int,
                     dst_w: int, dst_h: int) -> bytes:
    """Nearest-neighbour resample, matching the Frida callback's math.

    ``FUN_0045dad0`` is a box resampler; nearest-neighbour keeps the shim
    deterministic and cheap while preserving the image content.  The return
    value is ``dst_w * dst_h * channels`` bytes, row-major, top-left origin.
    """
    if channels not in (3, 4):
        raise ThumbShimError(f"channels must be 3 or 4, got {channels}")
    if min(src_w, src_h, dst_w, dst_h) <= 0:
        raise ThumbShimError("dimensions must be positive")
    need = src_w * src_h * channels
    if len(src) < need:
        raise ThumbShimError(f"source has {len(src)} bytes, need {need}")
    out = bytearray(dst_w * dst_h * channels)
    for y in range(dst_h):
        sy = min(src_h - 1, (y * src_h) // dst_h)
        srow = sy * src_w * channels
        drow = y * dst_w * channels
        for x in range(dst_w):
            sx = min(src_w - 1, (x * src_w) // dst_w)
            s = srow + sx * channels
            d = drow + x * channels
            out[d:d + channels] = src[s:s + channels]
    return bytes(out)


def screen_tga(width: int = 256, height: int = 256,
               color: tuple[int, int, int] = (0x60, 0x60, 0x60)) -> bytes:
    """Return an uncompressed 24-bpp top-left-origin TGA."""
    if not (0 < width <= 4096 and 0 < height <= 4096):
        raise ThumbShimError("TGA dimensions out of range")
    header = bytearray(18)
    header[2] = 2                      # uncompressed true-colour
    struct.pack_into("<HH", header, 12, width, height)
    header[16] = 24                    # bits per pixel
    header[17] = 0x20                  # top-left origin
    r, g, b = (int(c) & 0xFF for c in color)
    pixel = bytes((b, g, r))
    return bytes(header) + pixel * (width * height)


def write_screen_tga(path: str | Path, width: int = 256, height: int = 256,
                     color: tuple[int, int, int] = (0x60, 0x60, 0x60)) -> Path:
    p = Path(path)
    p.write_bytes(screen_tga(width, height, color))
    return p


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True, help="output Screen.tga path")
    ap.add_argument("--width", type=int, default=256)
    ap.add_argument("--height", type=int, default=256)
    ap.add_argument("--color", default="60,60,60",
                    help="R,G,B (default 60,60,60)")
    args = ap.parse_args(argv)
    try:
        color = tuple(int(x, 0) for x in args.color.split(","))
        if len(color) != 3:
            raise ValueError("expected R,G,B")
        p = write_screen_tga(args.out, args.width, args.height, color)  # type: ignore[arg-type]
    except (ValueError, ThumbShimError) as exc:
        ap.error(str(exc))
    print(f"wrote {p} ({args.width}x{args.height}, 24 bpp)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
