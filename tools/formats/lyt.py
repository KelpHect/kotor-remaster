#!/usr/bin/env python3
"""Strict parser for KotOR LYT (#MAXLAYOUT ASCII) area layout files (type 3000).

Grammar (verified against all 124 shipped LYT resources; CRLF or LF lines):

    #MAXLAYOUT ASCII
    filedependancy <name>            (optional, at most one)
    beginlayout
       roomcount <N>                 then N lines:  <room> <x> <y> <z>
       trackcount <N>                then N lines:  <track> <x> <y> <z>
       obstaclecount <N>             then N lines:  <obstacle> <x> <y> <z>
       doorhookcount <N>             then N lines:  <room> <node> <flag> <x> <y> <z> <qw> <qx> <qy> <qz>
    donelayout

Door-hook quaternions are W,X,Y,Z (verified against MDL door nodes and GIT
doors in docs/recon/04 and docs/recon/12).  The ``flag`` token is an
integer (0 in all shipped files).  Room names are resref-like tokens
(no whitespace, <= 16 bytes).  Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field


class LytError(ValueError):
    pass

HEADER = "#MAXLAYOUT ASCII"
MAX_NAME = 16


@dataclass(frozen=True)
class LytPoint:
    name: str
    x: float
    y: float
    z: float


@dataclass(frozen=True)
class DoorHook:
    room: str
    node: str
    flag: int
    x: float
    y: float
    z: float
    quat: tuple[float, float, float, float]  # W, X, Y, Z


@dataclass
class Lyt:
    filedependancy: str | None
    rooms: tuple[LytPoint, ...]
    tracks: tuple[LytPoint, ...]
    obstacles: tuple[LytPoint, ...]
    doorhooks: tuple[DoorHook, ...]
    warnings: list[str] = field(default_factory=list)


def _token_name(tok: str, what: str, lineno: int) -> str:
    if not tok:
        raise LytError(f"line {lineno}: empty {what}")
    if any(c in tok for c in " \t"):
        raise LytError(f"line {lineno}: {what} {tok!r} contains whitespace")
    if len(tok.encode("latin-1", "replace")) > MAX_NAME:
        raise LytError(f"line {lineno}: {what} {tok!r} longer than {MAX_NAME} bytes")
    return tok


def _float(tok: str, what: str, lineno: int) -> float:
    try:
        return float(tok)
    except ValueError:
        raise LytError(f"line {lineno}: {what} {tok!r} is not a float") from None


def _int(tok: str, what: str, lineno: int) -> int:
    try:
        return int(tok)
    except ValueError:
        raise LytError(f"line {lineno}: {what} {tok!r} is not an integer") from None


def _point(tokens: list[str], lineno: int) -> LytPoint:
    if len(tokens) != 4:
        raise LytError(f"line {lineno}: expected 4 tokens (name x y z), got {len(tokens)}")
    return LytPoint(_token_name(tokens[0], "name", lineno),
                    _float(tokens[1], "x", lineno),
                    _float(tokens[2], "y", lineno),
                    _float(tokens[3], "z", lineno))


def parse_lyt(data: bytes, name: str = "<memory>") -> Lyt:
    """Parse and strictly validate a LYT payload."""
    text = data.decode("latin-1")
    if "\x00" in text:
        raise LytError(f"{name}: NUL byte in LYT text")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    rows = [(i + 1, ln.rstrip("\r").strip()) for i, ln in enumerate(lines)]
    rows = [(n, s) for n, s in rows if s]
    if not rows:
        raise LytError(f"{name}: empty LYT")
    if rows[0][1] != HEADER:
        raise LytError(f"{name}: first line must be {HEADER!r}, got {rows[0][1]!r}")
    rows = [(n, s) for n, s in rows[1:] if not s.startswith("#")]
    pos = 0
    filedependancy = None
    if pos < len(rows) and rows[pos][1].lower().startswith("filedependancy"):
        parts = rows[pos][1].split(None, 1)
        if len(parts) != 2 or not parts[1].strip():
            raise LytError(f"{name}: line {rows[pos][0]}: malformed filedependancy")
        filedependancy = parts[1].strip()
        pos += 1
    if pos >= len(rows) or rows[pos][1].lower() != "beginlayout":
        got = rows[pos][1] if pos < len(rows) else "EOF"
        raise LytError(f"{name}: expected 'beginlayout', got {got!r}")
    pos += 1

    def read_section(keyword: str) -> list[tuple[int, str]]:
        nonlocal pos
        if pos >= len(rows) or not rows[pos][1].lower().startswith(keyword):
            got = rows[pos][1] if pos < len(rows) else "EOF"
            raise LytError(f"{name}: expected '{keyword}', got {got!r}")
        lineno, text_line = rows[pos]
        parts = text_line.split()
        if len(parts) != 2:
            raise LytError(f"{name}: line {lineno}: '{keyword} <count>' expected")
        count = _int(parts[1], "count", lineno)
        if count < 0 or count > 1_000_000:
            raise LytError(f"{name}: line {lineno}: implausible count {count}")
        pos += 1
        out = []
        for _ in range(count):
            if pos >= len(rows):
                raise LytError(f"{name}: EOF inside '{keyword}' section (want {count})")
            out.append(rows[pos])
            pos += 1
        return out

    rooms = tuple(_point(r[1].split(), r[0]) for r in read_section("roomcount"))
    tracks = tuple(_point(r[1].split(), r[0]) for r in read_section("trackcount"))
    obstacles = tuple(_point(r[1].split(), r[0]) for r in read_section("obstaclecount"))
    hooks = []
    for lineno, s in read_section("doorhookcount"):
        tokens = s.split()
        if len(tokens) != 10:
            raise LytError(f"line {lineno}: doorhook needs 10 tokens "
                           f"(room node flag x y z qw qx qy qz), got {len(tokens)}")
        hooks.append(DoorHook(
            _token_name(tokens[0], "room", lineno),
            _token_name(tokens[1], "node", lineno),
            _int(tokens[2], "flag", lineno),
            _float(tokens[3], "x", lineno), _float(tokens[4], "y", lineno),
            _float(tokens[5], "z", lineno),
            (_float(tokens[6], "qw", lineno), _float(tokens[7], "qx", lineno),
             _float(tokens[8], "qy", lineno), _float(tokens[9], "qz", lineno))))
    if pos >= len(rows) or rows[pos][1].lower() != "donelayout":
        got = rows[pos][1] if pos < len(rows) else "EOF"
        raise LytError(f"{name}: expected 'donelayout', got {got!r}")
    pos += 1
    warnings: list[str] = []
    if pos != len(rows):
        warnings.append(f"{name}: {len(rows) - pos} trailing line(s) after donelayout "
                        f"(shipped quirk, e.g. m12aa)")
    seen: set[str] = set()
    for r in rooms:
        if r.name in seen:
            # shipped stunt LYTs repeat the placeholder name '****'
            warnings.append(f"{name}: duplicate room {r.name!r}")
        seen.add(r.name)
    for h in hooks:
        if h.room not in seen:
            # shipped quirk: stunt LYTs reference rooms from the real layout
            warnings.append(f"{name}: doorhook references unknown room {h.room!r}")
    return Lyt(filedependancy, rooms, tracks, obstacles, tuple(hooks), warnings)


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one LYT resource (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    lyt = parse_lyt(data, args.path)
    print(f"{args.path}: rooms={len(lyt.rooms)} tracks={len(lyt.tracks)} "
          f"obstacles={len(lyt.obstacles)} doorhooks={len(lyt.doorhooks)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
