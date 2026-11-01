"""Minimal pure-Python PNG writer (zlib + struct, stdlib only).

8-bit RGB (color type 2) and RGBA (color type 6) images, filter 0 per
scanline.  No external dependencies; used by both area_viewer and
gui_viewer.
"""

from __future__ import annotations

import struct
import zlib


class Image:
    """8-bit RGB(A) framebuffer."""

    __slots__ = ("w", "h", "channels", "pix")

    def __init__(self, w: int, h: int, channels: int = 3, fill=(0, 0, 0)):
        if channels not in (3, 4):
            raise ValueError("channels must be 3 or 4")
        self.w = int(w)
        self.h = int(h)
        self.channels = channels
        r, g, b = fill[0], fill[1], fill[2]
        if channels == 4:
            row = bytes((r, g, b, fill[3] if len(fill) > 3 else 255)) * self.w
        else:
            row = bytes((r, g, b)) * self.w
        self.pix = bytearray(row * self.h)

    def set(self, x: int, y: int, rgb, a: int | None = None):
        if 0 <= x < self.w and 0 <= y < self.h:
            i = (y * self.w + x) * self.channels
            self.pix[i] = rgb[0] & 0xFF
            self.pix[i + 1] = rgb[1] & 0xFF
            self.pix[i + 2] = rgb[2] & 0xFF
            if self.channels == 4 and a is not None:
                self.pix[i + 3] = a & 0xFF

    def get(self, x: int, y: int):
        i = (y * self.w + x) * self.channels
        if self.channels == 4:
            return (self.pix[i], self.pix[i + 1], self.pix[i + 2], self.pix[i + 3])
        return (self.pix[i], self.pix[i + 1], self.pix[i + 2])

    def blend(self, x: int, y: int, rgb, a: int):
        """Source-over alpha blend of one pixel."""
        if not (0 <= x < self.w and 0 <= y < self.h):
            return
        i = (y * self.w + x) * self.channels
        if a >= 255:
            self.pix[i] = rgb[0] & 0xFF
            self.pix[i + 1] = rgb[1] & 0xFF
            self.pix[i + 2] = rgb[2] & 0xFF
            if self.channels == 4:
                self.pix[i + 3] = 255
            return
        if a <= 0:
            return
        ia = 255 - a
        self.pix[i] = (rgb[0] * a + self.pix[i] * ia) // 255
        self.pix[i + 1] = (rgb[1] * a + self.pix[i + 1] * ia) // 255
        self.pix[i + 2] = (rgb[2] * a + self.pix[i + 2] * ia) // 255
        if self.channels == 4:
            self.pix[i + 3] = min(255, a + self.pix[i + 3] * ia // 255)


def _chunk(tag: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def encode_png(img: Image) -> bytes:
    """Encode an Image to PNG bytes (filter 0)."""
    sig = b"\x89PNG\r\n\x1a\n"
    color = 2 if img.channels == 3 else 6
    ihdr = struct.pack(">IIBBBBB", img.w, img.h, 8, color, 0, 0, 0)
    stride = img.w * img.channels
    raw = bytearray()
    for y in range(img.h):
        raw.append(0)
        raw += img.pix[y * stride:(y + 1) * stride]
    idat = zlib.compress(bytes(raw), 6)
    return sig + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", idat) + _chunk(b"IEND", b"")


def write_png(path: str, img: Image) -> int:
    data = encode_png(img)
    with open(path, "wb") as f:
        f.write(data)
    return len(data)
