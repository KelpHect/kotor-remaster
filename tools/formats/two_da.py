#!/usr/bin/env python3
"""Read-only parser for KotOR 2DA tables (V2.b binary and V2.0 ASCII).

Wire layout (verified against all 209 tables shipped in data/2da.bif):

V2.b
  "2DA V2.b" + newline
  column names, each terminated by TAB, then one NUL
  u32 LE row count
  one row label per row, each terminated by TAB (no NUL after the last)
  u16 LE cell offsets[row_count * column_count], row-major, relative to
    the start of the string table
  u16 LE string-table byte size
  string table: NUL-terminated CP1252/Latin-1 byte strings; offsets point
    at the first byte of a string.  A zero-length string is a NUL at the
    offset and is structurally EMPTY.  It is not the ASCII "****" marker.
  A cell with no value is stored as a zero-length string by the shipped
  files; the KotOR accessors treat "" and "****" as equivalent "missing".

V2.0
  "2DA V2.0" line
  optional "Default: <value>" line
  optional blank lines
  column header tokens (spaces/tabs, '"' quotes spaces/tabs)
  rows: row label token followed by cell tokens; missing trailing cells
    are null-filled; "****" is the null marker (quoted or not); a quoted
    "" is an empty cell.

The parser never writes to its input.  It does not interpret numeric
columns; cells are strings plus a structural null flag.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass, field
from typing import Optional, Sequence

VERSION_V2B = "V2.b"
VERSION_V2A = "V2.0"
MAGIC_V2B = b"2DA V2.b"
MAGIC_V2A = b"2DA V2.0"
NULL_MARKER = "****"
MAX_ROWS = 1_000_000
MAX_COLUMNS = 65_535


class TwoDAError(ValueError):
    """Malformed 2DA data with a byte offset when one is known."""

    def __init__(self, reason: str, offset: Optional[int] = None, table: Optional[str] = None):
        self.reason = reason
        self.offset = offset
        self.table = table
        where = ""
        if table:
            where += f"{table}: "
        if offset is not None:
            where += f"0x{offset:x}: "
        super().__init__(where + reason)


@dataclass(frozen=True)
class Cell:
    """One cell.

    ``text`` is the decoded string ("" for a zero-length stored string).
    ``null`` marks the ASCII "****" placeholder or a null-filled missing
    trailing cell.  Empty and null are therefore structurally distinct.
    """

    text: str = ""
    null: bool = False

    @property
    def is_empty(self) -> bool:
        return not self.null and self.text == ""

    def as_text(self) -> str:
        return NULL_MARKER if self.null else self.text


@dataclass(frozen=True)
class Row:
    label: str
    cells: tuple[Cell, ...]


@dataclass
class TwoDA:
    version: str
    columns: tuple[str, ...]
    rows: tuple[Row, ...]
    default: Optional[str] = None
    layout: dict[str, int] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)
    name: Optional[str] = None

    @property
    def row_count(self) -> int:
        return len(self.rows)

    @property
    def column_count(self) -> int:
        return len(self.columns)

    def column_index(self, column: int | str) -> int:
        if isinstance(column, int):
            if not 0 <= column < self.column_count:
                raise IndexError(f"column index {column} outside table")
            return column
        try:
            return self.columns.index(column)
        except ValueError:
            raise KeyError(f"no column {column!r}") from None

    def row_index(self, row: int | str) -> int:
        if isinstance(row, int):
            if not 0 <= row < self.row_count:
                raise IndexError(f"row index {row} outside table")
            return row
        for i, entry in enumerate(self.rows):
            if entry.label == row:
                return i
        raise KeyError(f"no row label {row!r}")

    def cell(self, row: int | str, column: int | str) -> Cell:
        return self.rows[self.row_index(row)].cells[self.column_index(column)]

    def counts(self) -> dict[str, int]:
        cells = self.row_count * self.column_count
        empty = null = 0
        for entry in self.rows:
            for cell in entry.cells:
                if cell.null:
                    null += 1
                elif cell.text == "":
                    empty += 1
        return {
            "rows": self.row_count,
            "columns": self.column_count,
            "cells": cells,
            "empty_cells": empty,
            "null_cells": null,
        }


def _decode(raw: bytes) -> str:
    # CP1252 is the KotOR text code page.  All shipped 2DA identifiers are
    # ASCII; latin-1 fallback keeps malformed bytes inspectable rather than
    # silently dropping them.
    try:
        return raw.decode("cp1252")
    except UnicodeDecodeError:
        return raw.decode("latin-1")


def _u32(data: bytes, offset: int, what: str, table: Optional[str]) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise TwoDAError(f"truncated {what}", offset, table)
    return struct.unpack_from("<I", data, offset)[0]


def _u16(data: bytes, offset: int, what: str, table: Optional[str]) -> int:
    if offset < 0 or offset + 2 > len(data):
        raise TwoDAError(f"truncated {what}", offset, table)
    return struct.unpack_from("<H", data, offset)[0]


def _find(data: bytes, needle: bytes, start: int, end: int | None = None) -> int:
    return data.find(needle, start, len(data) if end is None else end)


def parse_2da(data: bytes, name: str | None = None) -> TwoDA:
    """Parse one 2DA resource.  Raises :class:`TwoDAError` on malformed data."""
    if data.startswith(MAGIC_V2B):
        return _parse_v2b(data, name)
    if data.startswith(MAGIC_V2A):
        return _parse_v2a(data, name)
    if len(data) < 8:
        raise TwoDAError(f"short 2DA header ({len(data)} bytes)", 0, name)
    raise TwoDAError(f"unsupported 2DA signature {data[:8]!r}", 0, name)


def _parse_v2b(data: bytes, table: Optional[str]) -> TwoDA:
    if len(data) < len(MAGIC_V2B) + 1:
        raise TwoDAError("short V2.b header", len(data), table)
    if data[:8] != MAGIC_V2B:
        raise TwoDAError("bad V2.b signature", 0, table)
    pos = 8
    if data[pos:pos + 1] == b"\r":
        pos += 1
    if data[pos:pos + 1] != b"\n":
        raise TwoDAError("V2.b header not terminated by newline", pos, table)
    pos += 1

    # Column-name blob: TAB after every name, then one NUL.
    columns_end = _find(data, b"\x00", pos)
    if columns_end < 0:
        raise TwoDAError("unterminated column-name table", pos, table)
    column_blob = data[pos:columns_end]
    if column_blob:
        parts = column_blob.split(b"\t")
        if parts[-1] != b"":
            raise TwoDAError("last column name is not TAB-terminated", columns_end, table)
        raw_columns = parts[:-1]
        if len(raw_columns) > MAX_COLUMNS:
            raise TwoDAError(f"implausible column count {len(raw_columns)}", pos, table)
        for i, raw in enumerate(raw_columns):
            if not raw:
                raise TwoDAError(f"empty column name at index {i}", pos, table)
            if b"\x00" in raw:
                raise TwoDAError(f"NUL in column name at index {i}", pos, table)
        columns = tuple(_decode(raw) for raw in raw_columns)
    else:
        columns = ()
    columns_start = pos
    pos = columns_end + 1

    row_count = _u32(data, pos, "row count", table)
    row_count_offset = pos
    if row_count > MAX_ROWS:
        raise TwoDAError(f"implausible row count {row_count}", pos, table)
    pos += 4
    if row_count and not columns:
        raise TwoDAError("rows present but no columns", pos, table)

    # Row labels: each TAB-terminated, no terminating NUL after the last.
    labels: list[str] = []
    labels_start = pos
    for i in range(row_count):
        end = _find(data, b"\t", pos)
        if end < 0:
            raise TwoDAError(f"unterminated row label {i}", pos, table)
        labels.append(_decode(data[pos:end]))
        pos = end + 1
    labels_end = pos

    cell_count = row_count * len(columns)
    offsets_start = pos
    offsets_end = offsets_start + cell_count * 2
    if offsets_end + 2 > len(data):
        raise TwoDAError(
            f"truncated cell-offset table (need {cell_count * 2} bytes)", offsets_start, table)
    offsets = struct.unpack_from(f"<{cell_count}H", data, offsets_start) if cell_count else ()
    pos = offsets_end
    string_size = _u16(data, pos, "string-table size", table)
    string_size_offset = pos
    pos += 2
    string_start = pos
    string_end = string_start + string_size
    if string_end > len(data):
        raise TwoDAError(
            f"string table [{string_start}+{string_size}] exceeds file {len(data)}",
            string_size_offset, table)

    warnings: list[str] = []
    if string_end != len(data):
        warnings.append(
            f"{len(data) - string_end} trailing byte(s) after declared string table")
    if len(set(columns)) != len(columns):
        warnings.append("duplicate column name(s)")
    if len(set(labels)) != len(labels):
        warnings.append("duplicate row label(s)")

    string_blob = data[string_start:string_end]
    covered = bytearray(string_size)
    cells: list[Cell] = []
    for i, off in enumerate(offsets):
        if off >= string_size:
            raise TwoDAError(
                f"cell offset {i} ({off}) outside string table of size {string_size}",
                offsets_start + i * 2, table)
        end = string_blob.find(b"\x00", off)
        if end < 0:
            raise TwoDAError(
                f"cell offset {i} ({off}) points to unterminated string",
                string_start + off, table)
        text = _decode(string_blob[off:end])
        # A zero-length stored string is structurally empty, not the ASCII
        # "****" placeholder.  Literal "****" text is preserved as text.
        cells.append(Cell(text=text, null=False))
        covered[off:end + 1] = b"\x01" * (end - off + 1)
    referenced_bytes = sum(covered)

    rows = tuple(
        Row(label=labels[r], cells=tuple(cells[r * len(columns):(r + 1) * len(columns)]))
        for r in range(row_count)
    )
    layout = {
        "header_size": columns_start,
        "column_table_offset": columns_start,
        "column_table_size": columns_end - columns_start + 1,
        "row_count_offset": row_count_offset,
        "row_label_table_offset": labels_start,
        "row_label_table_size": labels_end - labels_start,
        "cell_offset_table_offset": offsets_start,
        "cell_offset_table_size": cell_count * 2,
        "string_size_offset": string_size_offset,
        "string_table_offset": string_start,
        "string_table_size": string_size,
        "referenced_string_bytes": referenced_bytes,
        "unreferenced_string_bytes": string_size - referenced_bytes,
        "declared_file_size": string_end,
        "minimum_cell_offset": min(offsets) if offsets else 0,
        "maximum_cell_offset": max(offsets) if offsets else 0,
    }
    return TwoDA(
        version=VERSION_V2B,
        columns=columns,
        rows=rows,
        layout=layout,
        warnings=warnings,
        name=table,
    )


def _tokenize_ascii_line(line: str, line_offset: int, table: Optional[str]):
    """Split one ASCII line into (text, was_quoted) tokens.

    Spaces/tabs separate tokens; double quotes group spaces/tabs.  The
    opening quote must be at a token boundary and must be closed.
    """
    tokens: list[tuple[str, bool]] = []
    i = 0
    n = len(line)
    while i < n:
        ch = line[i]
        if ch in " \t":
            i += 1
            continue
        if ch == '"':
            i += 1
            buf: list[str] = []
            closed = False
            while i < n:
                if line[i] == '"':
                    closed = True
                    i += 1
                    break
                buf.append(line[i])
                i += 1
            if not closed:
                raise TwoDAError("unterminated quoted cell", line_offset, table)
            if i < n and line[i] not in " \t":
                raise TwoDAError(
                    "characters after closing quote before separator", line_offset + i, table)
            tokens.append(("".join(buf), True))
        else:
            start = i
            while i < n and line[i] not in " \t":
                i += 1
            tokens.append((line[start:i], False))
    return tokens


def _split_ascii_lines(data: bytes):
    """Yield (decoded line without CR/LF, byte offset of line start)."""
    offset = 0
    for raw in data.splitlines(keepends=True):
        body = raw.rstrip(b"\r\n")
        yield _decode(body), offset
        offset += len(raw)


def _parse_v2a(data: bytes, table: Optional[str]) -> TwoDA:
    bom = 0
    if data.startswith(b"\xef\xbb\xbf"):
        bom = 3
    lines = list(_split_ascii_lines(data[bom:]))
    if not lines:
        raise TwoDAError("empty V2.0 file", bom, table)
    first_text, first_off = lines[0]
    if first_text.strip() != MAGIC_V2A.decode("ascii"):
        raise TwoDAError("bad V2.0 header line", bom + first_off, table)

    default: Optional[str] = None
    header_tokens: list[tuple[str, bool]] | None = None
    header_offset = 0
    row_lines: list[tuple[list[tuple[str, bool]], int]] = []

    for text, off in lines[1:]:
        stripped = text.strip()
        if not stripped:
            continue
        if header_tokens is None and stripped.lower().startswith("default:"):
            value = stripped[len("default:"):].strip()
            default = value
            continue
        tokens = _tokenize_ascii_line(text, bom + off, table)
        if not tokens:
            continue
        if header_tokens is None:
            header_tokens = tokens
            header_offset = bom + off
        else:
            row_lines.append((tokens, bom + off))

    if not header_tokens:
        raise TwoDAError("V2.0 file has no column header", bom + first_off, table)
    columns = tuple(text for text, _quoted in header_tokens)
    for i, name in enumerate(columns):
        if not name:
            raise TwoDAError(f"empty column name at index {i}", header_offset, table)
    warnings: list[str] = []
    if len(set(columns)) != len(columns):
        warnings.append("duplicate column name(s)")

    rows: list[Row] = []
    width = len(columns)
    for tokens, off in row_lines:
        label = tokens[0][0]
        values = tokens[1:]
        if len(values) > width:
            raise TwoDAError(
                f"row {label!r} has {len(values)} cells for {width} columns", off, table)
        if len(values) < width:
            warnings.append(
                f"row {label!r} has {len(values)} cells; null-filled {width - len(values)}")
        cells: list[Cell] = []
        for text_value, _quoted in values:
            if text_value == NULL_MARKER:
                cells.append(Cell(text=NULL_MARKER, null=True))
            else:
                cells.append(Cell(text=text_value, null=False))
        cells.extend(Cell(text=NULL_MARKER, null=True) for _ in range(width - len(values)))
        rows.append(Row(label=label, cells=tuple(cells)))

    labels = [row.label for row in rows]
    if len(set(labels)) != len(labels):
        warnings.append("duplicate row label(s)")
    layout = {
        "header_size": bom + first_off + len(lines[0][0]),
        "column_header_offset": header_offset,
        "row_count": len(rows),
    }
    return TwoDA(
        version=VERSION_V2A,
        columns=columns,
        rows=tuple(rows),
        default=default,
        layout=layout,
        warnings=warnings,
        name=table,
    )


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Parse one 2DA file and print structure only.")
    ap.add_argument("path", help="2DA file path (read-only)")
    ap.add_argument("--json", action="store_true", help="emit JSON structure summary")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    try:
        table = parse_2da(data, name=args.path)
    except TwoDAError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    summary = {
        "version": table.version,
        "columns": list(table.columns),
        "counts": table.counts(),
        "warnings": table.warnings,
        "layout": table.layout,
    }
    if args.json:
        print(json.dumps(summary, indent=2))
    else:
        c = summary["counts"]
        print(f"{args.path}: {table.version} rows={c['rows']} columns={c['columns']} "
              f"cells={c['cells']} empty={c['empty_cells']} null={c['null_cells']}")
        print("columns: " + ", ".join(table.columns))
    return 0


if __name__ == "__main__":
    sys.exit(main())
