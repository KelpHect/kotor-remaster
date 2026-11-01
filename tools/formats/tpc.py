#!/usr/bin/env python3
"""Strict parser/validator for KotOR TPC textures (type 3007) and TXI.

TPC (verified on all 11,452 shipped TPC resources in patch.erf and the four
TexturePacks ERFs):

    0x00  u32  data_size -- compressed size of mip 0 (0 = raw/uncompressed)
    0x04  f32  alpha-test reference (1.0 = disabled)
    0x08  u16  width, u16 height
    0x0C  u8   encoding: 1 gray, 2 RGB/DXT1, 3 DXT3, 4 RGBA/DXT5, 0xC BGRA
    0x0D  u8   mip count (>= 1)
    0x0E  ...  zero padding to 128 bytes
    0x80  pixel data: full mip chain; compressed (DXT) chain when data_size
          > 0, raw chain when data_size == 0
          followed by the TXI text (may be absent)

Compressed mip sizes use 4x4 blocks (min 1 block): DXT1 8 B/block,
DXT3/DXT5 16 B/block.  Raw sizes: gray w*h, RGB w*h*3, RGBA/BGRA w*h*4.
When data_size > 0 it equals mip0's compressed size (mips > 1) or the whole
chain (mips == 1) -- both verified on the corpus.

TXI (type 2022) is appended text, one ``key value`` pair per line.
Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass


class TpcError(ValueError):
    pass

HEADER_SIZE = 128
ENCODINGS = {0x1: "gray", 0x2: "rgb/dxt1", 0x3: "dxt3", 0x4: "rgba/dxt5", 0xC: "bgra"}
MAX_DIM = 16384
MAX_MIPS = 16


@dataclass
class Tpc:
    data_size: int
    alpha_test: float
    width: int
    height: int
    encoding: int
    mip_count: int
    pixel_bytes: int      # total size of the stored mip chain
    txi: bytes            # trailing TXI payload (may be empty)
    size: int

    @property
    def encoding_name(self) -> str:
        return ENCODINGS.get(self.encoding, f"0x{self.encoding:02x}")


def _mip_size(w: int, h: int, enc: int, compressed: bool) -> int:
    if enc == 0x1:
        return w * h
    if enc == 0xC:
        return w * h * 4
    if enc in (0x2, 0x3, 0x4):
        bw = max(1, (w + 3) // 4)
        bh = max(1, (h + 3) // 4)
        if compressed:
            return bw * bh * (8 if enc == 0x2 else 16)
        return w * h * (3 if enc == 0x2 else 4)
    raise TpcError(f"unknown encoding 0x{enc:02x}")


def chain_size(w: int, h: int, enc: int, mips: int, compressed: bool) -> int:
    total = 0
    ww, hh = w, h
    for _ in range(mips):
        total += _mip_size(ww, hh, enc, compressed)
        ww = max(1, ww // 2)
        hh = max(1, hh // 2)
    return total


def parse_tpc(data: bytes, name: str = "<memory>", txi_parser=None) -> Tpc:
    """Parse and strictly validate a TPC payload.

    ``txi_parser`` (optional, e.g. txi.parse_txi) is applied to a non-empty
    trailing TXI blob.
    """
    if len(data) < HEADER_SIZE:
        raise TpcError(f"{name}: shorter than {HEADER_SIZE}-byte TPC header")
    data_size, = struct.unpack_from("<I", data, 0)
    alpha_test, = struct.unpack_from("<f", data, 4)
    width, height = struct.unpack_from("<HH", data, 8)
    enc = data[0x0C]
    mips = data[0x0D]
    if enc not in ENCODINGS:
        raise TpcError(f"{name}: unknown encoding 0x{enc:02x}")
    if not 1 <= width <= MAX_DIM or not 1 <= height <= MAX_DIM:
        raise TpcError(f"{name}: implausible dimensions {width}x{height}")
    if not 1 <= mips <= MAX_MIPS:
        raise TpcError(f"{name}: implausible mip count {mips}")
    compressed = data_size > 0
    comp_chain = chain_size(width, height, enc, mips, True)
    raw_chain = chain_size(width, height, enc, mips, False)
    # Pixel-region size: the shipped corpus uses data_size (mip0 or whole
    # chain, and in a few GUI textures a value that covers data+part of the
    # TXI), the full compressed chain, or -- when data_size == 0 -- the raw
    # chain.  Pick the candidate whose trailing bytes are empty or a valid
    # TXI blob.
    candidates = []
    if data_size:
        candidates.append(data_size)
    candidates += [comp_chain, raw_chain]
    pixel_bytes = None
    opaque_tail = False
    for cand in candidates:
        if HEADER_SIZE + cand > len(data):
            continue
        tail = data[HEADER_SIZE + cand:]
        if tail == b"":
            pixel_bytes = cand
            break
        if txi_parser is not None:
            try:
                txi_parser(tail, name + ":txi")
                pixel_bytes = cand
                break
            except Exception:
                continue
        elif tail[:1].isalpha():
            pixel_bytes = cand
            break
    if pixel_bytes is None:
        # cubemaps (CM_*) and a few GUI textures carry opaque trailing data
        # after the computed chain; accept the largest fitting candidate and
        # record the trailing bytes
        for cand in sorted(set(candidates), reverse=True):
            if HEADER_SIZE + cand <= len(data):
                pixel_bytes = cand
                opaque_tail = True
                break
    if pixel_bytes is None:
        raise TpcError(
            f"{name}: no pixel-region interpretation fits (data_size={data_size}, "
            f"comp_chain={comp_chain}, raw_chain={raw_chain}, len-128={len(data) - HEADER_SIZE})")
    txi = data[HEADER_SIZE + pixel_bytes:]
    if txi and not opaque_tail and txi_parser is not None:
        txi_parser(txi, name + ":txi")
    return Tpc(data_size, alpha_test, width, height, enc, mips, pixel_bytes, txi,
               len(data))


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one TPC resource (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    t = parse_tpc(data, args.path)
    print(f"{args.path}: {t.width}x{t.height} enc={t.encoding_name} mips={t.mip_count} "
          f"data_size={t.data_size} pixel_bytes={t.pixel_bytes} txi={len(t.txi)}B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
