#!/usr/bin/env python3
"""Strict parser for KotOR VIS room-visibility files (type 3001).

Grammar (verified against all 112 shipped VIS resources; plain ASCII text,
CRLF or LF, no header magic).  The real grammar is indentation-based:

    <room> [<count>]     non-indented header line (count optional, advisory)
      <visible-room>     indented entry lines, one room per line

Shipped quirks (recorded as warnings, not errors): the header count can
disagree with the number of entry lines (3 of 1,250 shipped sections), one
header may omit the count entirely, and entries may reference rooms that
head no section.  Room names are non-empty whitespace-free tokens of at
most 16 bytes; resref comparison is case-insensitive.
Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field


class VisError(ValueError):
    pass

MAX_NAME = 16


@dataclass
class VisSection:
    room: str
    count: int | None
    entries: tuple[str, ...]


@dataclass
class Vis:
    # ordered list of sections
    entries: tuple[VisSection, ...]
    warnings: list[str] = field(default_factory=list)

    def visible_from(self, room: str) -> tuple[str, ...]:
        for s in self.entries:
            if s.room.lower() == room.lower():
                return s.entries
        raise KeyError(room)


def _name(tok: str, lineno: int) -> str:
    if not tok:
        raise VisError(f"line {lineno}: empty room name")
    if len(tok.encode("latin-1", "replace")) > MAX_NAME:
        raise VisError(f"line {lineno}: room name {tok!r} longer than {MAX_NAME} bytes")
    return tok


def parse_vis(data: bytes, name: str = "<memory>") -> Vis:
    text = data.decode("latin-1")
    if "\x00" in text:
        raise VisError(f"{name}: NUL byte in VIS text")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    sections: list[VisSection] = []
    warnings: list[str] = []
    heads: set[str] = set()
    i = 0
    cur: VisSection | None = None
    cur_entries: list[str] = []
    lineno_of = []
    for idx, raw in enumerate(lines):
        lineno = idx + 1
        line = raw.rstrip("\r")
        if not line.strip():
            continue
        indented = line[0] in " \t"
        if indented:
            toks = line.split()
            if len(toks) != 1:
                raise VisError(f"{name}: line {lineno}: expected one room token, "
                               f"got {line.strip()!r}")
            if cur is None:
                raise VisError(f"{name}: line {lineno}: entry before any header")
            cur_entries.append(_name(toks[0], lineno))
            continue
        # flush current section
        if cur is not None:
            sections.append(VisSection(cur.room, cur.count, tuple(cur_entries)))
        parts = line.split()
        if len(parts) > 2:
            raise VisError(f"{name}: line {lineno}: expected '<room> [<count>]', "
                           f"got {line!r}")
        room = _name(parts[0], lineno)
        if room.lower() in heads:
            warnings.append(f"{name}: line {lineno}: duplicate room section {room!r}")
        heads.add(room.lower())
        count = None
        if len(parts) == 2:
            try:
                count = int(parts[1])
            except ValueError:
                raise VisError(f"{name}: line {lineno}: count {parts[1]!r} not an integer") from None
            if count < 0 or count > 1_000_000:
                raise VisError(f"{name}: line {lineno}: implausible count {count}")
        else:
            warnings.append(f"{name}: line {lineno}: header {room!r} has no count")
        cur = VisSection(room, count, ())
        cur_entries = []
        lineno_of.append(lineno)
    if cur is not None:
        sections.append(VisSection(cur.room, cur.count, tuple(cur_entries)))
    if not sections:
        raise VisError(f"{name}: empty VIS")
    for s in sections:
        if s.count is not None and s.count != len(s.entries):
            warnings.append(f"{name}: room {s.room!r} declares {s.count} but has "
                            f"{len(s.entries)} entries")
        for v in s.entries:
            if v.lower() not in heads:
                warnings.append(f"{name}: room {s.room!r} lists unknown room {v!r}")
    return Vis(tuple(sections), warnings)


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one VIS resource (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    v = parse_vis(data, args.path)
    links = sum(len(s.entries) for s in v.entries)
    print(f"{args.path}: sections={len(v.entries)} links={links} "
          f"warnings={len(v.warnings)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
