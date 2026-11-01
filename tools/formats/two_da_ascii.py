#!/usr/bin/env python3
"""2DA writer support: byte-identical V2.b re-encode and V2.0 ASCII writer.

Builds on the existing reader ``tools/formats/two_da.py`` (read-only import;
that module is unchanged).  Two writers:

* :func:`write_2da_v2b` -- re-encodes a parsed table to binary ``2DA V2.b``.
  The string table is rebuilt in row-major first-use order with duplicate
  strings sharing one offset; verified **byte-identical on all 209 shipped
  tables** (see docs/recon/18_FORMAT_MATRIX.md).  Structural null cells
  (ASCII ``****``) have no V2.b representation and are written as the
  zero-length string (the KotOR accessors treat both as missing; the
  structural distinction is preserved in the parsed model, not on wire).
* :func:`write_2da_v2a` -- writes ASCII ``2DA V2.0`` (for mod support):
  header line, optional ``Default:`` line, space-separated columns (quoted
  when they contain whitespace), one row per line, ``****`` for null
  cells, quoted cells when needed, CRLF line endings.

Round-trip: ``parse_2da(write_2da_v2b(t))`` is semantically equal for V2.b
inputs and byte-identical on the shipped corpus; ``parse_2da(write_2da_v2a(t))``
is semantically equal to ``t`` for both versions.
Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import os
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import two_da  # noqa: E402  (existing reader, read-only import)

TwoDAError = two_da.TwoDAError


def _encode_cell(text: str) -> bytes:
    try:
        return text.encode("cp1252")
    except UnicodeEncodeError:
        return text.encode("latin-1")


def write_2da_v2b(table: "two_da.TwoDA") -> bytes:
    """Re-encode a parsed table as binary V2.b (byte-identical policy)."""
    if table.version != two_da.VERSION_V2B and table.version != two_da.VERSION_V2A:
        raise TwoDAError(f"unsupported table version {table.version!r}")
    out = bytearray(two_da.MAGIC_V2B + b"\n")
    for col in table.columns:
        raw = _encode_cell(col)
        if not raw or b"\t" in raw or b"\x00" in raw:
            raise TwoDAError(f"column name {col!r} not encodable in V2.b")
        out += raw + b"\t"
    out += b"\x00"
    rows = table.rows
    for row in rows:
        if len(row.cells) != len(table.columns):
            raise TwoDAError(
                f"row {row.label!r} has {len(row.cells)} cells for "
                f"{len(table.columns)} columns")
    out += struct.pack("<I", len(rows))
    for row in rows:
        raw = _encode_cell(row.label)
        if not raw or b"\t" in raw or b"\x00" in raw:
            raise TwoDAError(f"row label {row.label!r} not encodable in V2.b")
        out += raw + b"\t"
    # string table: row-major first-use dedup (the shipped layout)
    strings = bytearray()
    offsets_map: dict[bytes, int] = {}
    cell_offsets: list[int] = []
    for row in rows:
        for cell in row.cells:
            raw = b"" if cell.null else _encode_cell(cell.text)
            off = offsets_map.get(raw)
            if off is None:
                off = len(strings)
                offsets_map[raw] = off
                strings += raw + b"\x00"
            cell_offsets.append(off)
    if len(strings) > 0xFFFF:
        raise TwoDAError(f"string table {len(strings)} bytes exceeds u16")
    for off in cell_offsets:
        if off > 0xFFFF:
            raise TwoDAError(f"cell offset {off} exceeds u16")
        out += struct.pack("<H", off)
    out += struct.pack("<H", len(strings))
    out += strings
    return bytes(out)


def _quote(token: str) -> str:
    if token == "" or any(c in token for c in " \t\""):
        return '"' + token.replace('"', "") + '"'
    return token


def write_2da_v2a(table: "two_da.TwoDA", default: str | None = None,
                  newline: bytes = b"\r\n") -> bytes:
    """Encode a parsed table as ASCII V2.0 (mod-friendly text format)."""
    if default is None:
        default = table.default
    out = bytearray(b"2DA V2.0" + newline)
    if default is not None:
        out += b"Default: " + _encode_cell(default) + newline
    out += b" ".join(_quote(c).encode("latin-1") for c in table.columns) + newline
    for row in table.rows:
        cells = [row.label] + [c.as_text() for c in row.cells]
        out += b" ".join(_quote(c).encode("latin-1") for c in cells) + newline
    return bytes(out)


def convert_to_ascii(data: bytes, name: str | None = None) -> bytes:
    """Parse any 2DA (V2.b or V2.0) and re-emit it as ASCII V2.0."""
    return write_2da_v2a(two_da.parse_2da(data, name))


def roundtrip_v2b(data: bytes, name: str | None = None) -> bytes:
    """Parse V2.b bytes and re-encode them (byte-identical policy)."""
    return write_2da_v2b(two_da.parse_2da(data, name))


# ---------------------------------------------------------------------------
# NUL-separated V2.b variant found in rims/*.rim (26 shipped tables)
# ---------------------------------------------------------------------------

def parse_2da_nul_variant(data: bytes, name: str | None = None) -> "two_da.TwoDA":
    """Parse the NUL-separated V2.b layout used by 2DA resources inside
    rims/*.rim: same as V2.b but column names and row labels are each
    NUL-terminated (no TAB separators, no extra terminator NUL).

    The column/label blob boundary is recovered by search: the first split
    after which u32 row count, labels, u16 cell offsets and the u16 string
    table tile the file exactly is the valid parse.
    """
    if not data.startswith(two_da.MAGIC_V2B + b"\n"):
        raise TwoDAError("not a V2.b header", 0, name)
    start = len(two_da.MAGIC_V2B) + 1
    # candidate split points: every NUL boundary in the column blob region
    nuls = []
    p = start
    while True:
        p = data.find(b"\x00", p)
        if p < 0:
            break
        nuls.append(p + 1)
        p += 1
        if p > len(data) - 8:
            break
    for cols_end in nuls:
        blob = data[start:cols_end]
        columns = tuple(two_da._decode(t) for t in blob.split(b"\x00") if t != b"")
        if not columns:
            continue
        for skip_col_nul in (True, False):  # rim variant: one extra NUL after the column blob
            rpos = cols_end + 1 if (skip_col_nul and cols_end < len(data)
                                    and data[cols_end] == 0) else cols_end
            try:
                row_count = struct.unpack_from("<I", data, rpos)[0]
            except struct.error:
                continue
            if row_count > two_da.MAX_ROWS or (row_count and not columns):
                continue
            pos = rpos + 4
            labels = []
            try:
                for _ in range(row_count):
                    end = data.index(b"\x00", pos)
                    labels.append(two_da._decode(data[pos:end]))
                    pos = end + 1
            except ValueError:
                continue
            cell_count = row_count * len(columns)
            if pos + cell_count * 2 + 2 > len(data):
                continue
            offsets = struct.unpack_from(f"<{cell_count}H", data, pos) if cell_count else ()
            pos += cell_count * 2
            string_size = struct.unpack_from("<H", data, pos)[0]
            pos += 2
            string_start = pos
            if string_start + string_size != len(data):
                continue
            blob2 = data[string_start:]
            cells = []
            try:
                for off in offsets:
                    if off >= string_size:
                        raise TwoDAError("offset outside string table")
                    end = blob2.find(b"\x00", off)
                    if end < 0:
                        raise TwoDAError("unterminated string")
                    cells.append(two_da.Cell(text=two_da._decode(blob2[off:end]), null=False))
            except TwoDAError:
                continue
            rows = tuple(
                two_da.Row(label=labels[r],
                           cells=tuple(cells[r * len(columns):(r + 1) * len(columns)]))
                for r in range(row_count))
            return two_da.TwoDA(version=two_da.VERSION_V2B, columns=columns, rows=rows,
                                name=name)
    raise TwoDAError("no valid NUL-separated V2.b parse", 0, name)


def write_2da_v2b_nul(table: "two_da.TwoDA") -> bytes:
    """Encode as the NUL-separated V2.b variant (rim layout)."""
    out = bytearray(two_da.MAGIC_V2B + b"\n")
    for col in table.columns:
        raw = _encode_cell(col)
        if not raw or b"\x00" in raw:
            raise TwoDAError(f"column name {col!r} not encodable")
        out += raw + b"\x00"
    out += b"\x00"  # one extra NUL terminates the column blob in the rim variant
    rows = table.rows
    for row in rows:
        if len(row.cells) != len(table.columns):
            raise TwoDAError(f"row {row.label!r} cell/column mismatch")
    out += struct.pack("<I", len(rows))
    for row in rows:
        raw = _encode_cell(row.label)
        if not raw or b"\x00" in raw:
            raise TwoDAError(f"row label {row.label!r} not encodable")
        out += raw + b"\x00"
    strings = bytearray()
    offsets_map: dict[bytes, int] = {}
    cell_offsets: list[int] = []
    for row in rows:
        for cell in row.cells:
            raw = b"" if cell.null else _encode_cell(cell.text)
            off = offsets_map.get(raw)
            if off is None:
                off = len(strings)
                offsets_map[raw] = off
                strings += raw + b"\x00"
            cell_offsets.append(off)
    if len(strings) > 0xFFFF:
        raise TwoDAError("string table exceeds u16")
    for off in cell_offsets:
        out += struct.pack("<H", off)
    out += struct.pack("<H", len(strings))
    out += strings
    return bytes(out)


def parse_2da_any(data: bytes, name: str | None = None):
    """Parse either V2.b layout (TAB-separated base BIFs or NUL-separated
    RIM variant) or ASCII V2.0.  Returns (table, layout) with layout in
    {'v2b-tab', 'v2b-nul', 'v2a'}."""
    try:
        return two_da.parse_2da(data, name), "v2b-tab"
    except TwoDAError:
        pass
    if data.startswith(two_da.MAGIC_V2B):
        return parse_2da_nul_variant(data, name), "v2b-nul"
    return two_da.parse_2da(data, name), "v2a"


def write_2da_v2b_any(table: "two_da.TwoDA", layout: str) -> bytes:
    if layout == "v2b-nul":
        return write_2da_v2b_nul(table)
    return write_2da_v2b(table)


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="2DA writer tools")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("to-ascii", help="convert a 2DA resource to ASCII V2.0")
    p.add_argument("inp")
    p.add_argument("out")
    p = sub.add_parser("reencode-v2b", help="parse and re-encode V2.b (byte-identical)")
    p.add_argument("inp")
    p.add_argument("out")
    args = ap.parse_args(argv)
    with open(args.inp, "rb") as f:
        data = f.read()
    if args.cmd == "to-ascii":
        blob = convert_to_ascii(data, args.inp)
    else:
        blob = roundtrip_v2b(data, args.inp)
    with open(args.out, "wb") as f:
        f.write(blob)
    print(f"{args.inp} -> {args.out} ({len(blob)} bytes, identical={blob == data})")
    return 0 if blob == data else 1


if __name__ == "__main__":
    sys.exit(main())
