#!/usr/bin/env python3
"""Strict parser/writer for KotOR TXI texture-info files (type 2022, and
the text blob appended to TPC textures).

Grammar (verified on all 5,615 shipped TXI resources plus the TXI blobs
appended to TPC textures): ASCII text, one ``key value`` pair per line,
CRLF or LF; blank lines are allowed and skipped; keys are any non-whitespace
token, case-preserving (shipped keys include ``Decal``, ``spacingR`` and
bare numbers like ``0.02``); a few shipped TXIs repeat a key (recorded as a
warning, first occurrence wins); values are the rest of the line (may be
empty).  Unknown keys are preserved, not rejected.
Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import re
import sys
from collections import OrderedDict


class TxiError(ValueError):
    pass


class TxiMap(OrderedDict):
    """Ordered key->value mapping carrying parser warnings."""

    def __init__(self):
        super().__init__()
        self.warnings: list[str] = []


_KEY = re.compile(r"^[^\s]+$")


def parse_txi(data: bytes, name: str = "<memory>") -> TxiMap:
    """Parse TXI text into an ordered key->value mapping."""
    try:
        text = data.decode("latin-1")
    except UnicodeDecodeError as exc:
        raise TxiError(f"{name}: not latin-1 decodable: {exc}") from None
    if "\x00" in text:
        raise TxiError(f"{name}: NUL byte in TXI text")
    out = TxiMap()
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    for i, raw in enumerate(lines):
        line = raw.rstrip("\r")
        if not line.strip():
            continue  # blank lines are allowed in shipped TXIs
        parts = line.split(None, 1)
        key = parts[0]
        if not _KEY.match(key):
            raise TxiError(f"{name}: line {i + 1}: bad key {key!r}")
        if key in out:
            out.warnings.append(f"{name}: line {i + 1}: duplicate key {key!r}")
            continue
        value = parts[1].strip() if len(parts) > 1 else ""
        out[key] = value
    return out


def write_txi(entries, newline: bytes = b"\r\n") -> bytes:
    """Serialize an ordered mapping (or iterable of pairs) to TXI bytes."""
    if hasattr(entries, "items"):
        entries = list(entries.items())
    out = bytearray()
    for key, value in entries:
        key = str(key)
        if not _KEY.match(key):
            raise TxiError(f"bad TXI key {key!r}")
        value = str(value)
        if any(c in value for c in "\r\n"):
            raise TxiError(f"TXI value for {key!r} contains a newline")
        out += key.encode("latin-1") + b" " + value.encode("latin-1") + newline
    return bytes(out)


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one TXI resource (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    t = parse_txi(data, args.path)
    print(f"{args.path}: {len(t)} keys, {len(t.warnings)} warnings: "
          + ", ".join(f"{k}={v}" for k, v in list(t.items())[:8]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
