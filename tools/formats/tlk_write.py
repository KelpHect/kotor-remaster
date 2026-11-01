#!/usr/bin/env python3
"""TLK (dialog.tlk) V3.0 writer support.

The existing reader ``tools/formats/tlk.py`` (unchanged here) is path-based
and decodes text; this module adds raw parse + two writers:

* :func:`parse_tlk_bytes` -- strict parse of a TLK V3.0 file into raw
  entries (flags, 16-byte sound resref, variances, text offset/length,
  sound length) plus the text blob.  Validates that every entry's text
  range lies inside the blob and that ranges do not overlap.
* :func:`tlk_bytes` -- layout-preserving re-encode: header (language,
  entry count, original text offset) + 40-byte entries rebuilt from the
  parsed fields + the text blob verbatim.  Byte-identical on the shipped
  dialog.tlk (49,265 entries; the 4,096-byte gap between the entry table
  and the text blob is preserved via the original text offset).
* :func:`tlk_repack` -- canonical writer for mod support (TSLPatcher-style
  appends): text blob re-laid as the concatenation of entry texts in
  order, offsets recomputed, text offset packed right after the entry
  table.  Semantic equality only, by construction.

Wire layout (docs/recon/02, verified on dialog.tlk):
    0x00 char[4] "TLK ", 0x04 char[4] "V3.0"
    0x08 u32 language, 0x0C u32 entry count, 0x10 u32 text offset
    0x14 entries: count x 40 B { u32 flags, char[16] sound resref,
          u32 volume variance, u32 pitch variance, u32 text offset
          (relative to text offset), u32 text length, f32 sound length }
    text blob at the header text offset.
Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import os
import struct
import sys
from dataclasses import dataclass


class TlkError(ValueError):
    pass

HEADER = struct.Struct("<4s4sIII")
ENTRY = struct.Struct("<I16sIIIIf")
MAGIC = b"TLK "
VERSION = b"V3.0"


@dataclass(frozen=True)
class TlkEntry:
    flags: int
    sound: bytes          # raw 16-byte resref field
    volume_variance: int
    pitch_variance: int
    text_offset: int      # relative to the text blob start
    text_length: int
    sound_length: float


@dataclass
class Tlk:
    language: int
    text_offset: int
    entries: tuple[TlkEntry, ...]
    blob: bytes           # raw text region [text_offset, EOF)

    @property
    def count(self) -> int:
        return len(self.entries)

    def text(self, i: int) -> bytes:
        e = self.entries[i]
        return self.blob[e.text_offset:e.text_offset + e.text_length]


def parse_tlk_bytes(data: bytes, name: str = "<memory>") -> Tlk:
    """Strictly parse a TLK V3.0 payload."""
    if len(data) < HEADER.size:
        raise TlkError(f"{name}: shorter than 20-byte TLK header")
    magic, version, language, count, text_offset = HEADER.unpack_from(data, 0)
    if magic != MAGIC or version != VERSION:
        raise TlkError(f"{name}: expected 'TLK V3.0', got {magic!r} {version!r}")
    if HEADER.size + count * ENTRY.size > len(data):
        raise TlkError(f"{name}: entry table [{HEADER.size},"
                       f"{HEADER.size + count * ENTRY.size}) exceeds file {len(data)}")
    if text_offset > len(data) or text_offset < HEADER.size + count * ENTRY.size:
        raise TlkError(f"{name}: text offset {text_offset} outside file / before table end")
    blob = data[text_offset:]
    entries = []
    covered: list[tuple[int, int]] = []
    for i in range(count):
        flags, sound, vol, pitch, off, length, slen = ENTRY.unpack_from(
            data, HEADER.size + i * ENTRY.size)
        if off + length > len(blob):
            raise TlkError(f"{name}: entry {i} text [{off},{off + length}) "
                           f"exceeds blob {len(blob)}")
        covered.append((off, off + length))
        entries.append(TlkEntry(flags, sound, vol, pitch, off, length, slen))
    covered.sort()
    for (a0, a1), (b0, _b1) in zip(covered, covered[1:]):
        if b0 < a1:
            raise TlkError(f"{name}: overlapping text ranges [{a0},{a1}) and [{b0},...)")
    return Tlk(language, text_offset, tuple(entries), blob)


def _entry_bytes(e: TlkEntry) -> bytes:
    return ENTRY.pack(e.flags, e.sound, e.volume_variance, e.pitch_variance,
                      e.text_offset, e.text_length, e.sound_length)


def tlk_bytes(t: Tlk) -> bytes:
    """Layout-preserving re-encode (byte-identical for well-formed input)."""
    out = bytearray(HEADER.pack(MAGIC, VERSION, t.language, len(t.entries),
                                t.text_offset))
    for e in t.entries:
        out += _entry_bytes(e)
    if len(out) > t.text_offset:
        raise TlkError("entry table overruns preserved text offset")
    out += b"\x00" * (t.text_offset - len(out))
    out += t.blob
    return bytes(out)


def tlk_repack(t: Tlk, texts: list[bytes] | None = None) -> bytes:
    """Canonical writer: re-lay the text blob from per-entry text bytes.

    ``texts[i]`` defaults to the entry's current text.  Offsets are
    recomputed and the blob is packed immediately after the entry table.
    """
    if texts is None:
        texts = [t.text(i) for i in range(len(t.entries))]
    if len(texts) != len(t.entries):
        raise TlkError("texts length must match entry count")
    blob = bytearray()
    entries = []
    for e, raw in zip(t.entries, texts):
        if not isinstance(raw, (bytes, bytearray)):
            raise TlkError("text entries must be bytes")
        entries.append(TlkEntry(e.flags, e.sound, e.volume_variance,
                                e.pitch_variance, len(blob), len(raw), e.sound_length))
        blob += raw
    text_offset = HEADER.size + len(entries) * ENTRY.size
    out = bytearray(HEADER.pack(MAGIC, VERSION, t.language, len(entries), text_offset))
    for e in entries:
        out += _entry_bytes(e)
    out += blob
    return bytes(out)


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="TLK writer tools")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("reencode", help="parse and re-encode (byte-identical)")
    p.add_argument("inp")
    p.add_argument("out")
    p = sub.add_parser("repack", help="canonical repack (semantic)")
    p.add_argument("inp")
    p.add_argument("out")
    args = ap.parse_args(argv)
    with open(args.inp, "rb") as f:
        data = f.read()
    t = parse_tlk_bytes(data, args.inp)
    blob = tlk_bytes(t) if args.cmd == "reencode" else tlk_repack(t)
    with open(args.out, "wb") as f:
        f.write(blob)
    print(f"{args.inp} -> {args.out}: entries={t.count} text_offset={t.text_offset} "
          f"identical={blob == data}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
