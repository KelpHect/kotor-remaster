#!/usr/bin/env python3
"""Read-only KotOR archive readers and small fixture writers (lane 05).

Supported layouts (all little-endian):

* ERF/MOD/SAV V1.0 -- 160-byte header, 24-byte keys, 8-byte resource records.
  KotOR places entryCount at 0x10, the key table at 0x14 and the resource
  table at 0x1C.  This is the layout of patch.erf, texture packs and save
  archives (SAVEGAME.sav is tagged "MOD ").
* RIM V1.0 -- 20-byte header (count at +12, entry table at +16), 32-byte
  entries {resref[16], type:u32, id:u32, offset:u32, size:u32}.
* BIFF V1 -- 20-byte header {varCount, fixedCount, offsetToVarTable}, 16-byte
  records {id, offset, size, type}.  Payloads are uncompressed.
* KEY V1 -- 64-byte header, 12-byte file records, 22-byte key records.  A key
  resource id encodes BIF index in bits 20..31 and table id in bits 0..19.

The writers are deliberately small and deterministic.  They are fixture
builders, not claimed byte-compatible engine writers.  They preserve entry
order and duplicate keys so the VFS tests can exercise first-match rules.

No function in this module writes to a path supplied as a game install; the
caller is responsible for choosing a lane-local output path.
"""
from __future__ import annotations

import hashlib
import io
import os
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence


class ArchiveError(ValueError):
    """Raised when an archive header or table violates a checked invariant."""


def normalize_resref(raw: str | bytes) -> str:
    """Return the lookup form of a resref: NUL-trimmed, latin-1, lower-case."""
    if isinstance(raw, bytes):
        raw = raw.split(b"\x00", 1)[0].decode("latin-1", "replace")
    else:
        raw = raw.split("\x00", 1)[0]
    return raw.lower()


@dataclass(frozen=True)
class ArchiveEntry:
    """One key-table entry.  Duplicate (resref, type) pairs are preserved."""

    resref: str
    res_type: int
    res_id: int
    offset: int
    size: int
    index: int

    @property
    def key(self) -> tuple[str, int]:
        return (normalize_resref(self.resref), self.res_type)


@dataclass
class Archive:
    path: str
    format: str
    magic: str
    version: str
    entries: list[ArchiveEntry]
    file_size: int
    header: dict = field(default_factory=dict)
    buffer: bytes | None = None

    def read_entry(self, entry: ArchiveEntry) -> bytes:
        if entry.offset < 0 or entry.size < 0:
            raise ArchiveError("negative resource range")
        if entry.offset + entry.size > self.file_size:
            raise ArchiveError(
                f"{self.path}: entry {entry.index} range "
                f"{entry.offset}+{entry.size} exceeds {self.file_size}"
            )
        if self.buffer is not None:
            return self.buffer[entry.offset:entry.offset + entry.size]
        with open(self.path, "rb") as f:
            f.seek(entry.offset)
            data = f.read(entry.size)
        if len(data) != entry.size:
            raise ArchiveError(f"{self.path}: short read for entry {entry.index}")
        return data

    def lookup(self, resref: str, res_type: int | None = None) -> ArchiveEntry | None:
        want = normalize_resref(resref)
        for e in self.entries:
            if e.key[0] == want and (res_type is None or e.res_type == res_type):
                return e
        return None

    def duplicates(self) -> dict[tuple[str, int], list[int]]:
        out: dict[tuple[str, int], list[int]] = {}
        for e in self.entries:
            out.setdefault(e.key, []).append(e.index)
        return {k: v for k, v in out.items() if len(v) > 1}


@dataclass(frozen=True)
class KeyEntry:
    resref: str
    res_type: int
    resid: int
    bif_index: int
    table_index: int
    index: int

    @property
    def key(self) -> tuple[str, int]:
        return (normalize_resref(self.resref), self.res_type)


@dataclass
class KeyFile:
    path: str
    bifs: list[dict]
    entries: list[KeyEntry]
    file_size: int
    stride: int
    header: dict = field(default_factory=dict)

    def lookup(self, resref: str, res_type: int | None = None) -> KeyEntry | None:
        want = normalize_resref(resref)
        for e in self.entries:
            if e.key[0] == want and (res_type is None or e.res_type == res_type):
                return e
        return None

    def duplicates(self) -> dict[tuple[str, int], list[int]]:
        out: dict[tuple[str, int], list[int]] = {}
        for e in self.entries:
            out.setdefault(e.key, []).append(e.index)
        return {k: v for k, v in out.items() if len(v) > 1}


@dataclass
class BifRecord:
    id: int
    offset: int
    size: int
    res_type: int
    index: int

    @property
    def key(self) -> tuple[int, int]:
        return (self.id, self.res_type)


@dataclass
class BifFile:
    path: str
    var_count: int
    fixed_count: int
    records: list[BifRecord]
    by_id: dict[int, BifRecord]
    file_size: int
    header: dict = field(default_factory=dict)

    def read_record(self, record: BifRecord) -> bytes:
        if record.offset + record.size > self.file_size:
            raise ArchiveError(
                f"{self.path}: BIF record {record.id} exceeds file"
            )
        with open(self.path, "rb") as f:
            f.seek(record.offset)
            data = f.read(record.size)
        if len(data) != record.size:
            raise ArchiveError(f"{self.path}: short read for BIF record {record.id}")
        return data


def _read_exact(f, offset: int, size: int) -> bytes:
    f.seek(offset)
    data = f.read(size)
    if len(data) != size:
        raise ArchiveError(f"short read at 0x{offset:x}: wanted {size}, got {len(data)}")
    return data


# ---------------------------------------------------------------------------
# readers
# ---------------------------------------------------------------------------


def _parse_erf(source, file_size: int, path: str, buffer: bytes | None = None) -> Archive:
    if file_size < 160:
        raise ArchiveError(f"{path}: shorter than 160-byte ERF header")
    head = _read_exact(source, 0, 160)
    magic = head[0:4]
    if magic not in (b"ERF ", b"MOD ", b"SAV "):
        raise ArchiveError(f"{path}: not an ERF/MOD/SAV file")
    version = head[4:8]
    (language_count, localized_size, entry_count, off_localized,
     off_keys, off_resources, build_year, build_day, desc_strref) = \
        struct.unpack_from("<9I", head, 8)
    if language_count > 1024:
        raise ArchiveError(f"{path}: implausible language count {language_count}")
    if entry_count > 1_000_000:
        raise ArchiveError(f"{path}: implausible entry count {entry_count}")
    if off_keys + entry_count * 24 > file_size:
        raise ArchiveError(f"{path}: key table exceeds file")
    if off_resources + entry_count * 8 > file_size:
        raise ArchiveError(f"{path}: resource table exceeds file")
    if language_count and off_localized >= file_size:
        raise ArchiveError(f"{path}: localized string table offset outside file")

    keys = []
    for i in range(entry_count):
        b = _read_exact(source, off_keys + i * 24, 24)
        resref = b[:16].split(b"\x00", 1)[0].decode("latin-1", "replace")
        res_id, res_type, unused = struct.unpack_from("<IHH", b, 16)
        keys.append((resref, res_id, res_type, unused))

    resources = []
    for i in range(entry_count):
        off, rsize = struct.unpack_from("<II", _read_exact(source, off_resources + i * 8, 8))
        if off + rsize > file_size:
            raise ArchiveError(
                f"{path}: resource {i} range {off}+{rsize} exceeds {file_size}"
            )
        resources.append((off, rsize))

    entries = [
        ArchiveEntry(resref=resref, res_type=res_type, res_id=res_id,
                     offset=off, size=rsize, index=i)
        for i, ((resref, res_id, res_type, _unused), (off, rsize))
        in enumerate(zip(keys, resources))
    ]
    header = {
        "language_count": language_count,
        "localized_string_size": localized_size,
        "entry_count": entry_count,
        "offset_to_localized_string": off_localized,
        "offset_to_key_list": off_keys,
        "offset_to_resource_list": off_resources,
        "build_year": build_year,
        "build_day": build_day,
        "description_strref": desc_strref,
        "header_size": 160,
    }
    return Archive(path=path, format="ERF", magic=magic.decode("latin-1"),
                   version=version.decode("latin-1"), entries=entries,
                   file_size=file_size, header=header, buffer=buffer)


def read_erf(path: str | os.PathLike) -> Archive:
    p = str(path)
    size = os.path.getsize(p)
    with open(p, "rb") as f:
        return _parse_erf(f, size, p)


def read_erf_bytes(data: bytes, name: str = "<memory>") -> Archive:
    return _parse_erf(io.BytesIO(data), len(data), name, buffer=data)


def _parse_rim(source, file_size: int, path: str) -> Archive:
    if file_size < 20:
        raise ArchiveError(f"{path}: shorter than 20-byte RIM header")
    head = _read_exact(source, 0, 20)
    if head[:4] != b"RIM ":
        raise ArchiveError(f"{path}: not a RIM file")
    version = head[4:8]
    reserved, entry_count, off_entries = struct.unpack_from("<3I", head, 8)
    if entry_count > 1_000_000:
        raise ArchiveError(f"{path}: implausible RIM entry count")
    if off_entries + entry_count * 32 > file_size:
        raise ArchiveError(f"{path}: RIM entry table exceeds file")
    entries = []
    for i in range(entry_count):
        b = _read_exact(source, off_entries + i * 32, 32)
        resref = b[:16].split(b"\x00", 1)[0].decode("latin-1", "replace")
        res_type, res_id, off, rsize = struct.unpack_from("<4I", b, 16)
        if off + rsize > file_size:
            raise ArchiveError(f"{path}: RIM entry {i} exceeds file")
        entries.append(ArchiveEntry(resref=resref, res_type=res_type,
                                    res_id=res_id, offset=off, size=rsize, index=i))
    return Archive(path=path, format="RIM", magic="RIM ",
                   version=version.decode("latin-1"), entries=entries,
                   file_size=file_size,
                   header={"reserved": reserved, "entry_count": entry_count,
                           "offset_to_entries": off_entries, "header_size": 20})


def read_rim(path: str | os.PathLike) -> Archive:
    p = str(path)
    size = os.path.getsize(p)
    with open(p, "rb") as f:
        return _parse_rim(f, size, p)


def read_archive(path: str | os.PathLike) -> Archive:
    """Dispatch ERF/MOD/SAV/RIM by magic; raises ArchiveError otherwise."""
    p = str(path)
    with open(p, "rb") as f:
        magic = f.read(4)
    if magic in (b"ERF ", b"MOD ", b"SAV "):
        return read_erf(p)
    if magic == b"RIM ":
        return read_rim(p)
    raise ArchiveError(f"{p}: not a supported ERF/RIM archive")


def read_bif(path: str | os.PathLike) -> BifFile:
    p = str(path)
    size = os.path.getsize(p)
    if size < 20:
        raise ArchiveError(f"{p}: shorter than 20-byte BIF header")
    with open(p, "rb") as f:
        head = f.read(20)
        if head[:4] != b"BIFF" or head[4:8] != b"V1  ":
            raise ArchiveError(f"{p}: not a BIFF V1 file")
        var_count, fixed_count, off_var = struct.unpack_from("<3I", head, 8)
        off_fixed = off_var + var_count * 16
        if off_var < 20 or off_var > size:
            raise ArchiveError(f"{p}: variable table offset outside file")
        if var_count and off_var + var_count * 16 > size:
            raise ArchiveError(f"{p}: variable table exceeds file")
        if fixed_count and off_fixed + fixed_count * 16 > size:
            raise ArchiveError(f"{p}: fixed table exceeds file")
        records: list[BifRecord] = []
        by_id: dict[int, BifRecord] = {}
        index = 0
        for table_off, count, kind in ((off_var, var_count, "var"),
                                       (off_fixed, fixed_count, "fixed")):
            for i in range(count):
                rid, off, rsize, rtype = struct.unpack(
                    "<4I", _read_exact(f, table_off + i * 16, 16))
                if off + rsize > size:
                    raise ArchiveError(f"{p}: {kind} resource {i} exceeds file")
                rec = BifRecord(id=rid, offset=off, size=rsize, res_type=rtype,
                                index=index)
                records.append(rec)
                by_id.setdefault(rid, rec)  # first duplicate wins
                index += 1
    return BifFile(path=p, var_count=var_count, fixed_count=fixed_count,
                   records=records, by_id=by_id, file_size=size,
                   header={"offset_to_var_table": off_var,
                           "offset_to_fixed_table": off_fixed})


def read_key(path: str | os.PathLike) -> KeyFile:
    p = str(path)
    size = os.path.getsize(p)
    if size < 64:
        raise ArchiveError(f"{p}: shorter than 64-byte KEY header")
    with open(p, "rb") as f:
        head = f.read(64)
        if head[:4] != b"KEY " or head[4:8] != b"V1  ":
            raise ArchiveError(f"{p}: not a KEY V1 file")
        bif_count, key_count, off_files, off_keys = struct.unpack_from("<4I", head, 8)
        if bif_count > 4096 or key_count > 2_000_000:
            raise ArchiveError(f"{p}: implausible KEY counts")
        if off_files + bif_count * 12 > size:
            raise ArchiveError(f"{p}: KEY file table exceeds file")
        bifs = []
        for i in range(bif_count):
            declared_size, name_off, name_len, drives = struct.unpack(
                "<IIHH", _read_exact(f, off_files + i * 12, 12))
            name = _key_name(f, p, size, name_off, name_len, off_files)
            bifs.append({"index": i, "declared_size": declared_size,
                         "name": name, "drives": drives})
        key_blob = _read_exact(f, off_keys, size - off_keys)
    stride = 22
    if key_count and len(key_blob) % key_count == 0:
        padded = len(key_blob) // key_count
        if padded == 24:
            stride = 24
    if key_count and off_keys + key_count * stride > size:
        raise ArchiveError(f"{p}: KEY resource table exceeds file")
    entries = []
    for i in range(key_count):
        base = i * stride
        if base + 22 > len(key_blob):
            raise ArchiveError(f"{p}: KEY table truncated")
        resref = key_blob[base:base + 16].split(b"\x00", 1)[0].decode("latin-1", "replace")
        res_type, resid = struct.unpack_from("<HI", key_blob, base + 16)
        entries.append(KeyEntry(resref=resref, res_type=res_type, resid=resid,
                                bif_index=(resid >> 20) & 0xFFF,
                                table_index=resid & 0xFFFFF, index=i))
    return KeyFile(path=p, bifs=bifs, entries=entries, file_size=size,
                   stride=stride,
                   header={"bif_count": bif_count, "key_count": key_count,
                           "offset_to_file_table": off_files,
                           "offset_to_key_table": off_keys})


def _key_name(f, path: str, size: int, name_off: int, name_len: int,
              off_files: int) -> str:
    if name_len <= 0 or name_len > 260:
        raise ArchiveError(f"{path}: invalid KEY file name length {name_len}")
    candidates = [name_off, off_files + name_off]
    for cand in candidates:
        if cand < 0 or cand + name_len > size:
            continue
        raw = _read_exact(f, cand, name_len).split(b"\x00", 1)[0]
        text = raw.decode("latin-1", "replace").replace("\\", "/")
        if text.lower().endswith(".bif"):
            return text
    raise ArchiveError(f"{path}: cannot resolve KEY file name at 0x{name_off:x}")


def bif_path_for(game_dir: str | os.PathLike, name: str) -> str:
    """Resolve a KEY file name (usually data\\foo.bif) under a game directory."""
    gd = Path(game_dir)
    rel = name.replace("\\", "/")
    if rel.lower().startswith("data/"):
        return str(gd / rel)
    return str(gd / "data" / os.path.basename(rel))


def sha256_file(path: str | os.PathLike) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# deterministic fixture writers
# ---------------------------------------------------------------------------


def _align4(value: int) -> int:
    return (value + 3) & ~3


def _encode_resref(resref: str) -> bytes:
    raw = resref.encode("latin-1")
    if b"\x00" in raw:
        raise ArchiveError("resref contains NUL")
    if len(raw) > 16:
        raise ArchiveError(f"resref {resref!r} longer than 16 bytes")
    return raw.ljust(16, b"\x00")


def erf_bytes(entries: Sequence[tuple[str, int, bytes]],
              magic: bytes = b"ERF ", version: bytes = b"V1.0",
              build_year: int = 0, build_day: int = 0,
              description_strref: int = 0) -> bytes:
    """Build KotOR-layout ERF/MOD/SAV bytes, preserving order and duplicates."""
    n = len(entries)
    if n > 1_000_000:
        raise ArchiveError("too many ERF entries")
    header_size = 160
    off_keys = header_size
    off_resources = off_keys + n * 24
    data_start = _align4(off_resources + n * 8)
    keys = bytearray()
    resource_table = bytearray()
    payload = bytearray()
    cursor = data_start
    for i, (resref, res_type, data) in enumerate(entries):
        if not 0 <= int(res_type) <= 0xFFFF:
            raise ArchiveError(f"ERF type {res_type} does not fit u16")
        if not isinstance(data, (bytes, bytearray)):
            raise ArchiveError("ERF payload must be bytes")
        keys += _encode_resref(resref)
        keys += struct.pack("<IHH", i, int(res_type), 0)
        cursor = _align4(cursor)
        if cursor > len(payload) + data_start:
            payload += b"\x00" * (cursor - (len(payload) + data_start))
        resource_table += struct.pack("<II", cursor, len(data))
        payload += bytes(data)
        cursor += len(data)
    header = bytearray()
    header += magic[:4].ljust(4, b" ")
    header += version[:4].ljust(4, b" ")
    header += struct.pack("<9I", 0, 0, n, header_size, off_keys, off_resources,
                          int(build_year) & 0xFFFFFFFF,
                          int(build_day) & 0xFFFFFFFF,
                          int(description_strref) & 0xFFFFFFFF)
    header += b"\x00" * (header_size - len(header))
    blob = bytes(header) + bytes(keys) + bytes(resource_table)
    if len(blob) < data_start:
        blob += b"\x00" * (data_start - len(blob))
    blob += bytes(payload)
    return blob


def write_erf(path: str | os.PathLike, entries: Sequence[tuple[str, int, bytes]],
              magic: bytes = b"ERF ", version: bytes = b"V1.0",
              build_year: int = 0, build_day: int = 0,
              description_strref: int = 0) -> str:
    """Write a KotOR-layout ERF/MOD/SAV preserving entry order and duplicates."""
    p = str(path)
    blob = erf_bytes(entries, magic=magic, version=version,
                     build_year=build_year, build_day=build_day,
                     description_strref=description_strref)
    with open(p, "wb") as f:
        f.write(blob)
    return p


def write_rim(path: str | os.PathLike,
              entries: Sequence[tuple[str, int, bytes]]) -> str:
    """Write a RIM V1.0 archive using the observed 120-byte entry offset."""
    p = str(path)
    n = len(entries)
    header_size = 120
    data_start = _align4(header_size + n * 32)
    table = bytearray()
    payload = bytearray()
    cursor = data_start
    for i, (resref, res_type, data) in enumerate(entries):
        if not isinstance(data, (bytes, bytearray)):
            raise ArchiveError("RIM payload must be bytes")
        cursor = _align4(cursor)
        if cursor > len(payload) + data_start:
            payload += b"\x00" * (cursor - (len(payload) + data_start))
        table += _encode_resref(resref)
        table += struct.pack("<4I", int(res_type) & 0xFFFFFFFF, i, cursor, len(data))
        payload += bytes(data)
        cursor += len(data)
    header = bytearray()
    header += b"RIM V1.0"
    header += struct.pack("<3I", 0, n, header_size)
    header += b"\x00" * (header_size - len(header))
    blob = bytes(header) + bytes(table)
    if len(blob) < data_start:
        blob += b"\x00" * (data_start - len(blob))
    blob += bytes(payload)
    with open(p, "wb") as f:
        f.write(blob)
    return p


def write_bif(path: str | os.PathLike,
              resources: Sequence[tuple[int, int, bytes]]) -> str:
    """Write a fixed-table-only BIFF V1 file.  resources=(id,type,data)."""
    p = str(path)
    n = len(resources)
    data_start = _align4(20 + n * 16)
    table = bytearray()
    payload = bytearray()
    cursor = data_start
    for rid, rtype, data in resources:
        if not isinstance(data, (bytes, bytearray)):
            raise ArchiveError("BIF payload must be bytes")
        cursor = _align4(cursor)
        if cursor > len(payload) + data_start:
            payload += b"\x00" * (cursor - (len(payload) + data_start))
        table += struct.pack("<4I", int(rid) & 0xFFFFFFFF, cursor,
                             len(data), int(rtype) & 0xFFFFFFFF)
        payload += bytes(data)
        cursor += len(data)
    header = bytearray()
    header += b"BIFFV1  "
    header += struct.pack("<3I", 0, n, 20)
    blob = bytes(header) + bytes(table)
    if len(blob) < data_start:
        blob += b"\x00" * (data_start - len(blob))
    blob += bytes(payload)
    with open(p, "wb") as f:
        f.write(blob)
    return p


def write_key(path: str | os.PathLike,
              bif_files: Sequence[dict],
              keys: Sequence[tuple[str, int, int, int]]) -> str:
    """Write a KEY V1 file.

    bif_files: [{"name": "data\\\\base.bif", "size": 123}]
    keys:      [(resref, type, bif_index, table_index)]
    """
    p = str(path)
    off_files = 64
    name_blob = bytearray()
    file_table = bytearray()
    names_start = off_files + len(bif_files) * 12
    for b in bif_files:
        name = b["name"].encode("latin-1") + b"\x00"
        file_table += struct.pack("<IIHH", int(b.get("size", 0)) & 0xFFFFFFFF,
                                  names_start + len(name_blob), len(name),
                                  int(b.get("drives", 1)) & 0xFFFF)
        name_blob += name
    off_keys = names_start + len(name_blob)
    key_table = bytearray()
    for resref, res_type, bif_index, table_index in keys:
        if not (0 <= bif_index <= 0xFFF):
            raise ArchiveError("bif index does not fit 12 bits")
        if not (0 <= table_index <= 0xFFFFF):
            raise ArchiveError("table index does not fit 20 bits")
        resid = (int(bif_index) << 20) | int(table_index)
        key_table += _encode_resref(resref)
        key_table += struct.pack("<HI", int(res_type) & 0xFFFF, resid)
    header = bytearray()
    header += b"KEY V1  "
    header += struct.pack("<4I", len(bif_files), len(keys), off_files, off_keys)
    header += b"\x00" * (64 - len(header))
    blob = bytes(header) + bytes(file_table) + bytes(name_blob) + bytes(key_table)
    with open(p, "wb") as f:
        f.write(blob)
    return p


def write_key_bif(tree_root: str | os.PathLike,
                  resources: Sequence[tuple[str, int, bytes]],
                  bif_rel: str = "data\\base.bif") -> tuple[str, str]:
    """Create a one-BIF base tier: data/base.bif plus chitin.key."""
    root = Path(tree_root)
    bif_rel_fs = bif_rel.replace("\\", "/")
    bif_abs = root / bif_rel_fs
    bif_abs.parent.mkdir(parents=True, exist_ok=True)
    write_bif(bif_abs, [(i, rtype, data) for i, (_rr, rtype, data)
                        in enumerate(resources)])
    key_abs = root / "chitin.key"
    write_key(key_abs,
              [{"name": bif_rel, "size": bif_abs.stat().st_size, "drives": 1}],
              [(rr, rtype, 0, i) for i, (rr, rtype, _data)
               in enumerate(resources)])
    return str(key_abs), str(bif_abs)
