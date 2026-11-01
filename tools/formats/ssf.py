#!/usr/bin/env python3
"""Strict parser for KotOR SSF (sound set) files (type 2060).

KotOR SSF V1.1 (verified on all 211 shipped SSF resources in the base BIFs
and rims/global.rim; NOT the NWN layout -- there is no filename table):

    0x00  char[8] "SSF V1.1"
    0x08  u32     constant 12 in every shipped file
    0x0C  u32[]   StrRef entries to end of file (0xFFFFFFFF = empty slot)

Shipped files are 172 bytes (41 u32 total) or 208 bytes (50 u32 total).
Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass


class SsfError(ValueError):
    pass

MAGIC = b"SSF V1.1"
LEAD = 12
EMPTY = 0xFFFFFFFF


@dataclass
class Ssf:
    strrefs: tuple[int, ...]
    size: int


def parse_ssf(data: bytes, name: str = "<memory>") -> Ssf:
    if len(data) < 12:
        raise SsfError(f"{name}: shorter than 12-byte SSF header")
    if data[:8] != MAGIC:
        raise SsfError(f"{name}: bad magic {data[:8]!r}")
    if (len(data) - 8) % 4:
        raise SsfError(f"{name}: payload size {len(data) - 8} not a multiple of 4")
    lead, = struct.unpack_from("<I", data, 8)
    if lead != LEAD:
        raise SsfError(f"{name}: lead u32 {lead} != {LEAD}")
    strrefs = struct.unpack_from(f"<{(len(data) - 12) // 4}I", data, 12)
    return Ssf(strrefs, len(data))


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one SSF resource (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    s = parse_ssf(data, args.path)
    filled = sum(1 for x in s.strrefs if x != EMPTY)
    print(f"{args.path}: slots={len(s.strrefs)} filled={filled}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
