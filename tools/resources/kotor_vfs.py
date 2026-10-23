#!/usr/bin/env python3
"""Read-only KotOR resource VFS resolver with the documented five passes.

Precedence model (docs/recon/02_TECHNICAL_NOTES.md section 7):

  1. Directories: Override/, streammusic/, streamwaves/, movies/
  2. ERFs tagged 1: patch.erf, Override/textures.erf
  3. RIMs: active module area/static/adx RIMs, menu/chargen RIMs, rims/global.rim
  4. ERFs tagged 2: saved module ERF, module .mod, TexturePacks, lips/*.mod,
     players
  5. chitin.key + BIFs

Rules implemented:
* resrefs are NUL-trimmed, latin-1, lower-case; matching is case-insensitive;
* within one table the first duplicate (resref, type) entry wins;
* within one pass the table added last is searched first (mount/search order is
  explicit in code); lower pass numbers always win;
* a save module ERF only shadows the resources it actually stores;
* the first-launch streamwaves/Override loose-WAV quirk is modeled as a
  directory-table special case.

The intra-pass order for passes 3 and 4 and the texture-quality mapping are
inferred, not measured.  ``mounts`` and every Resolution carry notes.  The
resolver never writes to the game directory.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parents[2]
for _p in (Path(__file__).resolve().parent, REPO_ROOT / "tools" / "inventory"):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

import kotor_archive as ka  # noqa: E402
from kotor_types import TYPE_INFO  # noqa: E402

PASS_NAMES = {
    1: "directories",
    2: "erfs-tagged-1",
    3: "rims",
    4: "erfs-tagged-2",
    5: "key-and-bifs",
}

EXT_TO_TYPE = {
    "tga": 3, "wav": 4, "bmu": 8, "txt": 10, "mdl": 2002, "nss": 2009,
    "ncs": 2010, "mod": 2011, "are": 2012, "ifo": 2014, "bic": 2015,
    "wok": 2016, "2da": 2017, "txi": 2022, "git": 2023, "bti": 2024,
    "uti": 2025, "utc": 2027, "dlg": 2029, "utt": 2032, "dds": 2033,
    "uts": 2035, "gff": 2037, "fac": 2038, "ute": 2040, "utd": 2042,
    "utp": 2044, "gui": 2047, "utm": 2051, "dwk": 2052, "pwk": 2053,
    "jrl": 2056, "sav": 2057, "utw": 2058, "ssf": 2060, "bik": 2063,
    "lyt": 3000, "vis": 3001, "rim": 3002, "pth": 3003, "lip": 3004,
    "tpc": 3007, "mdx": 3008, "erf": 9997, "bif": 9998, "key": 9999,
}


def normalize_resref(raw: str | bytes) -> str:
    return ka.normalize_resref(raw)


def type_label(res_type: int | None) -> str:
    if res_type is None:
        return "UNKNOWN"
    return TYPE_INFO.get(int(res_type), ("UNKNOWN",))[0]


def ext_to_type(ext: str) -> int | None:
    return EXT_TO_TYPE.get(ext.lower().lstrip("."))


def find_path(base: Path, relative: str) -> Path | None:
    """Case-insensitive path resolution under ``base``."""
    current = base
    for part in relative.replace("\\", "/").split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            current = current.parent
            continue
        exact = current / part
        if exact.exists():
            current = exact
            continue
        try:
            matches = [c for c in current.iterdir() if c.name.lower() == part.lower()]
        except OSError:
            return None
        if not matches:
            return None
        current = matches[0]
    return current


def relative_label(game_dir: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(game_dir.resolve()).as_posix()
    except Exception:
        return str(path)


@dataclass(frozen=True)
class Entry:
    resref: str
    res_type: int
    index: int
    offset: int | None = None
    size: int = 0
    source: str = ""

    @property
    def key(self) -> tuple[str, int]:
        return (normalize_resref(self.resref), self.res_type)


@dataclass
class TableHit:
    entry: Entry
    duplicate_indices: list[int]
    error: str | None = None


class Table:
    kind = "table"

    def __init__(self, name: str, path: Path, game_dir: Path, exists: bool | None = None):
        self.name = name
        self.path = path
        self.game_dir = game_dir
        self.exists = path.exists() if exists is None else exists
        self.entries: list[Entry] = []
        self._index: dict[tuple[str, int], Entry] = {}
        self._dups: dict[tuple[str, int], list[int]] = {}
        self.notes: list[str] = []

    def _finish_index(self) -> None:
        self._index = {}
        self._dups = {}
        for e in self.entries:
            if e.key not in self._index:
                self._index[e.key] = e
            self._dups.setdefault(e.key, []).append(e.index)

    def lookup(self, key: tuple[str, int]) -> TableHit | None:
        e = self._index.get(key)
        if e is None:
            return None
        return TableHit(entry=e, duplicate_indices=list(self._dups.get(key, [e.index])))

    def read(self, entry: Entry) -> bytes:
        raise NotImplementedError

    def info(self) -> dict:
        return {"kind": self.kind, "name": self.name,
                "path": str(self.path), "exists": self.exists,
                "entries": len(self.entries), "notes": list(self.notes)}


class DirectoryTable(Table):
    kind = "directory"

    def __init__(self, name: str, path: Path, game_dir: Path, recursive: bool = True):
        super().__init__(name, path, game_dir)
        if not path.is_dir():
            return
        files = []
        if recursive:
            for p in path.rglob("*"):
                if p.is_file():
                    files.append(p)
        else:
            files = [p for p in path.iterdir() if p.is_file()]
        files.sort(key=lambda p: relative_label(game_dir, p).lower())
        for p in files:
            stem = p.stem
            rtype = ext_to_type(p.suffix)
            if rtype is None:
                continue
            if len(stem.encode("latin-1", "replace")) > 16:
                self.notes.append(f"ignored long resref {p.name}")
                continue
            self.entries.append(Entry(resref=stem, res_type=rtype,
                                      index=len(self.entries), size=p.stat().st_size,
                                      source=str(p)))
        self._finish_index()

    def read(self, entry: Entry) -> bytes:
        return Path(entry.source).read_bytes()


class ArchiveTable(Table):
    def __init__(self, name: str, path: Path, game_dir: Path,
                 archive: ka.Archive | None = None, kind: str | None = None):
        super().__init__(name, path, game_dir, exists=archive is not None or path.exists())
        self.archive = archive
        if self.archive is None and path.exists():
            self.archive = ka.read_archive(path)
        self.kind = kind or (self.archive.format.lower() if self.archive else "archive")
        if self.archive:
            for e in self.archive.entries:
                self.entries.append(Entry(resref=e.resref, res_type=e.res_type,
                                          index=e.index, offset=e.offset,
                                          size=e.size, source=str(path)))
        self._finish_index()

    def lookup(self, key: tuple[str, int]) -> TableHit | None:
        hit = super().lookup(key)
        if hit is None or self.archive is None:
            return hit
        return hit

    def read(self, entry: Entry) -> bytes:
        if self.archive is None:
            raise FileNotFoundError(self.name)
        return self.archive.read_entry(self.archive.entries[entry.index])


class KeyTable(Table):
    kind = "key"

    def __init__(self, name: str, path: Path, game_dir: Path):
        super().__init__(name, path, game_dir)
        self.key: ka.KeyFile | None = None
        self._bifs: dict[int, ka.BifFile | None] = {}
        if not path.exists():
            return
        self.key = ka.read_key(path)
        for e in self.key.entries:
            self.entries.append(Entry(resref=e.resref, res_type=e.res_type,
                                      index=e.index, source=str(path)))
        self._finish_index()

    def _bif(self, bif_index: int) -> ka.BifFile | None:
        if bif_index in self._bifs:
            return self._bifs[bif_index]
        self._bifs[bif_index] = None
        if self.key is None or bif_index >= len(self.key.bifs):
            return None
        bp = ka.bif_path_for(self.game_dir, self.key.bifs[bif_index]["name"])
        try:
            self._bifs[bif_index] = ka.read_bif(bp)
        except Exception as exc:
            self.notes.append(f"BIF {bif_index} unreadable: {exc}")
            self._bifs[bif_index] = None
        return self._bifs[bif_index]

    def lookup(self, key: tuple[str, int]) -> TableHit | None:
        if self.key is None:
            return None
        entry = self._index.get(key)
        if entry is None:
            return None
        ke = self.key.entries[entry.index]
        bif = self._bif(ke.bif_index)
        if bif is None:
            return TableHit(entry=Entry(resref=ke.resref, res_type=ke.res_type,
                                       index=ke.index, source=""),
                            duplicate_indices=list(self._dups.get(key, [ke.index])),
                            error=f"BIF {ke.bif_index} missing/unreadable")
        rec = bif.by_id.get(ke.resid)
        if rec is None:
            rec = bif.by_id.get(ke.table_index)
        if rec is None:
            return TableHit(entry=Entry(resref=ke.resref, res_type=ke.res_type,
                                       index=ke.index, source=bif.path),
                            duplicate_indices=list(self._dups.get(key, [ke.index])),
                            error=f"BIF record {ke.resid}/{ke.table_index} missing")
        return TableHit(entry=Entry(resref=ke.resref, res_type=ke.res_type,
                                    index=ke.index, offset=rec.offset,
                                    size=rec.size, source=bif.path),
                        duplicate_indices=list(self._dups.get(key, [ke.index])))

    def read(self, entry: Entry) -> bytes:
        if self.key is None:
            raise FileNotFoundError(self.name)
        ke = self.key.entries[entry.index]
        bif = self._bif(ke.bif_index)
        if bif is None:
            raise FileNotFoundError(f"{self.name}: BIF {ke.bif_index}")
        rec = bif.by_id.get(ke.resid)
        if rec is None:
            rec = bif.by_id.get(ke.table_index)
        if rec is None:
            raise FileNotFoundError(f"{self.name}: BIF record {ke.resid}/{ke.table_index}")
        return bif.read_record(rec)


@dataclass
class Pass:
    number: int
    name: str
    tables: list[Table] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)


@dataclass
class VfsHit:
    pass_no: int
    pass_name: str
    table_order: int
    table: Table
    entry: Entry
    duplicate_indices: list[int]
    error: str | None = None

    @property
    def tier(self) -> str:
        if self.pass_no == 1:
            return f"directory:{self.table.name}"
        if self.pass_no == 2:
            return f"erf1:{self.table.name}"
        if self.pass_no == 3:
            return f"rim:{self.table.name}"
        if self.pass_no == 4:
            return f"erf2:{self.table.name}"
        return f"base:{self.table.name}"

    def info(self) -> dict:
        return {
            "pass": self.pass_no,
            "pass_name": self.pass_name,
            "tier": self.tier,
            "table": self.table.name,
            "table_kind": self.table.kind,
            "table_order": self.table_order,
            "entry_index": self.entry.index,
            "resref": self.entry.resref,
            "type": self.entry.res_type,
            "type_label": type_label(self.entry.res_type),
            "offset": self.entry.offset,
            "size": self.entry.size,
            "source": self.entry.source,
            "duplicate_indices": self.duplicate_indices,
            "error": self.error,
        }


@dataclass
class Resolution:
    resref: str
    res_type: int
    found: bool
    winner: VfsHit | None
    traces: list[VfsHit]
    tables_searched: int
    notes: list[str]

    def read_payload(self) -> bytes | None:
        if self.winner is None:
            return None
        return self.winner.table.read(self.winner.entry)

    def info(self, include_hash: bool = False) -> dict:
        out = {
            "key": {"resref": self.resref, "type": self.res_type,
                    "type_label": type_label(self.res_type)},
            "found": self.found,
            "winner": self.winner.info() if self.winner else None,
            "traces": [h.info() for h in self.traces],
            "tables_searched": self.tables_searched,
            "notes": list(self.notes),
        }
        if include_hash and self.found:
            data = self.read_payload()
            if data is not None:
                out["payload_size"] = len(data)
                out["payload_sha256"] = hashlib.sha256(data).hexdigest()
        return out


class Vfs:
    def __init__(self, game_dir: str | os.PathLike, active_module: str | None = None,
                 loaded_save: str | os.PathLike | None = None,
                 quality: str = "auto", first_launch: bool = False,
                 main_menu: bool = False, chargen: bool = False,
                 include_texture_packs: bool = True,
                 extra_rims: Iterable[str] = (), extra_erfs: Iterable[str] = (),
                 extra_dirs: Iterable[str] = ()):
        self.game_dir = Path(game_dir)
        self.active_module = self._root_name(active_module) if active_module else None
        self.loaded_save = Path(loaded_save) if loaded_save else None
        self.quality = quality.lower()
        self.first_launch = bool(first_launch)
        self.main_menu = bool(main_menu)
        self.chargen = bool(chargen)
        self.include_texture_packs = include_texture_packs
        self.extra_rims = list(extra_rims)
        self.extra_erfs = list(extra_erfs)
        self.extra_dirs = list(extra_dirs)
        self.notes: list[str] = [
            "five-pass precedence follows docs/recon/02_TECHNICAL_NOTES.md section 7",
            "intra-pass order for RIMs and ERFs tagged 2 is inferred; measure with lane 06",
            "texture-pack quality mapping is unverified; quality=auto assumes tpa",
        ]
        self.passes: list[Pass] = []
        self._build()

    @staticmethod
    def _root_name(name: str) -> str:
        n = Path(str(name)).name
        low = n.lower()
        for suffix in (".rim", ".mod", ".erf", ".sav"):
            if low.endswith(suffix):
                low = low[:-len(suffix)]
        if low.endswith("_s"):
            low = low[:-2]
        return low

    def _path(self, relative: str) -> Path:
        p = find_path(self.game_dir, relative)
        return p if p is not None else self.game_dir / relative.replace("\\", "/")

    def _add_dir(self, p: Pass, label: str, relative: str) -> None:
        path = self._path(relative)
        p.tables.append(DirectoryTable(label, path, self.game_dir))

    def _add_erf(self, p: Pass, label: str, relative: str,
                 archive: ka.Archive | None = None) -> None:
        path = self._path(relative)
        try:
            p.tables.append(ArchiveTable(label, path, self.game_dir, archive=archive))
        except Exception as exc:
            t = Table(label, path, self.game_dir)
            t.kind = "archive"
            t.notes.append(f"unreadable: {exc}")
            p.tables.append(t)

    def _add_rim(self, p: Pass, label: str, relative: str) -> None:
        self._add_erf(p, label, relative)

    def _build(self) -> None:
        # Pass 1: directories.  Override is head by default.  The loose-WAV
        # first-launch quirk is applied at lookup time.
        p1 = Pass(1, PASS_NAMES[1])
        for label, rel in (("Override", "Override"),
                           ("streammusic", "streammusic"),
                           ("streamwaves", "streamwaves"),
                           ("movies", "movies")):
            self._add_dir(p1, label, rel)
        for rel in self.extra_dirs:
            self._add_dir(p1, Path(rel).name, rel)
        p1.notes.append("Override first; first-launch WAV quirk promotes streamwaves for type 4")
        self.passes.append(p1)

        # Pass 2: ERFs tagged 1.  textures.erf is added after patch.erf, so it
        # is searched first under the last-added-first rule.
        p2 = Pass(2, PASS_NAMES[2])
        self._add_erf(p2, "textures.erf", "Override/textures.erf")
        self._add_erf(p2, "patch.erf", "patch.erf")
        for rel in self.extra_erfs:
            if rel.lower().endswith("textures.erf"):
                self._add_erf(p2, Path(rel).name, rel)
        p2.notes.append("Override/textures.erf searched before patch.erf (last-added-first)")
        self.passes.append(p2)

        # Pass 3: RIMs.  Search order assumes global/menu are mounted first and
        # module RIMs last; static is last of the module RIMs.
        p3 = Pass(3, PASS_NAMES[3])
        self._add_rim(p3, "global.rim", "rims/global.rim")
        if self.main_menu:
            self._add_rim(p3, "mainmenudx.rim", "rims/mainmenudx.rim")
            self._add_rim(p3, "mainmenu.rim", "rims/mainmenu.rim")
        if self.chargen:
            self._add_rim(p3, "chargendx.rim", "rims/chargendx.rim")
            self._add_rim(p3, "chargen.rim", "rims/chargen.rim")
        if self.active_module:
            root = self.active_module
            self._add_rim(p3, f"{root}_adx.rim", f"rims/{root}_adx.rim")
            self._add_rim(p3, f"{root}_a.rim", f"rims/{root}_a.rim")
            self._add_rim(p3, f"{root}.rim", f"modules/{root}.rim")
            self._add_rim(p3, f"{root}_adx.rim", f"modules/{root}_adx.rim")
            self._add_rim(p3, f"{root}_s.rim", f"modules/{root}_s.rim")
        for rel in self.extra_rims:
            self._add_rim(p3, Path(rel).name, rel)
        # Reorder them into last-added-first search order: static, adx, area,
        # menu, global.  Tables are appended above in roughly add order.
        if self.active_module:
            root = self.active_module
            wanted = [f"{root}_s.rim", f"{root}_adx.rim", f"{root}_a.rim",
                      f"{root}.rim", "mainmenudx.rim", "mainmenu.rim",
                      "chargendx.rim", "chargen.rim", "global.rim"]
            ordered = []
            for name in wanted:
                for t in p3.tables:
                    if t.name == name and t not in ordered:
                        ordered.append(t)
            p3.tables = ordered + [t for t in p3.tables if t not in ordered]
        p3.notes.append("static/adx/area RIMs searched before menu/global; inferred order")
        self.passes.append(p3)

        # Pass 4: ERFs tagged 2.  Search order: saved module state, module .mod,
        # texture packs, lips, players.  "Texture packs below module archives".
        p4 = Pass(4, PASS_NAMES[4])
        save_table = self._load_save_table()
        if save_table is not None:
            p4.tables.append(save_table)
        if self.active_module:
            self._add_erf(p4, f"{self.active_module}.mod",
                          f"modules/{self.active_module}.mod")
        if self.include_texture_packs:
            for qname, rel in self._quality_files():
                self._add_erf(p4, qname, rel)
            self._add_erf(p4, "swpc_tex_gui.erf", "TexturePacks/swpc_tex_gui.erf")
        if self.active_module:
            root = self.active_module
            self._add_erf(p4, f"{root}_loc.mod", f"lips/{root}_loc.mod")
        self._add_erf(p4, "localization.mod", "lips/localization.mod")
        players = self._path("players")
        if players.exists():
            self._add_erf(p4, "players", "players")
        for rel in self.extra_erfs:
            if rel.lower().endswith("textures.erf"):
                continue
            self._add_erf(p4, Path(rel).name, rel)
        p4.notes.append(
            "search order: saved module ERF, module .mod, texture packs, lips, players "
            "(inferred; module archives beat texture packs)")
        if self.loaded_save is not None and save_table is None:
            p4.notes.append("loaded save did not yield a nested module ERF")
        self.passes.append(p4)

        # Pass 5: chitin.key and BIFs.
        p5 = Pass(5, PASS_NAMES[5])
        key_path = self._path("chitin.key")
        p5.tables.append(KeyTable("chitin.key", key_path, self.game_dir))
        self.passes.append(p5)

    def _quality_files(self) -> list[tuple[str, str]]:
        if not self.include_texture_packs or self.quality == "none":
            return []
        known = {
            "tpa": ("swpc_tex_tpa.erf", "TexturePacks/swpc_tex_tpa.erf"),
            "tpb": ("swpc_tex_tpb.erf", "TexturePacks/swpc_tex_tpb.erf"),
            "tpc": ("swpc_tex_tpc.erf", "TexturePacks/swpc_tex_tpc.erf"),
        }
        if self.quality == "all":
            return [known[k] for k in ("tpa", "tpb", "tpc")]
        if self.quality in known:
            return [known[self.quality]]
        # auto/unknown: assume highest quality.  The harness measures this.
        return [known["tpa"]]

    def _load_save_table(self) -> Table | None:
        if self.loaded_save is None:
            return None
        p = self.loaded_save
        if p.is_dir():
            candidate = find_path(p, "SAVEGAME.sav")
            if candidate is None:
                return None
            p = candidate
        if not p.exists():
            return None
        try:
            outer = ka.read_erf(p)
        except Exception:
            return None
        candidates = [e for e in outer.entries if e.res_type == 2057]
        nested = None
        nested_name = None
        if not candidates:
            # A caller may pass a nested module ERF directly.
            nested = outer
            nested_name = p.stem
        else:
            chosen = None
            if self.active_module:
                chosen = next((e for e in candidates
                               if normalize_resref(e.resref) == self.active_module), None)
            if chosen is None and len(candidates) == 1:
                chosen = candidates[0]
            if chosen is not None:
                try:
                    data = outer.read_entry(chosen)
                    nested = ka.read_erf_bytes(data, f"{p.name}#{chosen.resref}")
                    nested_name = chosen.resref
                except Exception:
                    nested = None
        if nested is None:
            return None
        label = f"SAVEGAME.sav#{nested_name or 'module'}"
        table = ArchiveTable(label, p, self.game_dir, archive=nested, kind="save-erf")
        table.notes.append("saved module state; shadows only stored resources")
        return table

    # ------------------------------------------------------------------
    # lookup
    # ------------------------------------------------------------------

    def _pass1_order(self, res_type: int) -> list[Table]:
        tables = self.passes[0].tables
        if self.first_launch and res_type == 4:
            names = ["streamwaves", "Override", "streammusic", "movies"]
            return sorted(tables, key=lambda t: names.index(t.name)
                          if t.name in names else len(names))
        return tables

    def _tables_for(self, p: Pass, res_type: int) -> list[Table]:
        if p.number == 1:
            return self._pass1_order(res_type)
        return p.tables

    def trace(self, resref: str, res_type: int) -> Resolution:
        key = (normalize_resref(resref), int(res_type))
        traces: list[VfsHit] = []
        tried = 0
        for p in self.passes:
            for order, table in enumerate(self._tables_for(p, res_type)):
                if not table.exists:
                    continue
                tried += 1
                try:
                    hit = table.lookup(key)
                except Exception as exc:
                    table.notes.append(f"lookup failed: {exc}")
                    continue
                if hit is None:
                    continue
                traces.append(VfsHit(pass_no=p.number, pass_name=p.name,
                                     table_order=order, table=table,
                                     entry=hit.entry,
                                     duplicate_indices=hit.duplicate_indices,
                                     error=hit.error))
        notes = list(self.notes)
        if self.quality not in ("tpa", "tpb", "tpc", "all", "none"):
            notes.append("quality=auto used swpc_tex_tpa.erf; mapping unverified")
        if self.active_module:
            notes.append(f"active_module={self.active_module}")
        return Resolution(resref=key[0], res_type=key[1], found=bool(traces),
                          winner=traces[0] if traces else None,
                          traces=traces, tables_searched=tried, notes=notes)

    def resolve(self, resref: str, res_type: int) -> Resolution:
        return self.trace(resref, res_type)

    def mounts(self) -> dict:
        return {
            "game_dir": str(self.game_dir),
            "active_module": self.active_module,
            "loaded_save": str(self.loaded_save) if self.loaded_save else None,
            "quality": self.quality,
            "first_launch": self.first_launch,
            "passes": [
                {"pass": p.number, "name": p.name,
                 "notes": list(p.notes),
                 "tables": [t.info() for t in p.tables]}
                for p in self.passes
            ],
            "notes": list(self.notes),
        }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _parse_query(resref: str, type_arg: str | None) -> tuple[str, int | None]:
    if type_arg is not None:
        t = type_arg.strip()
        if t.isdigit():
            return resref, int(t)
        return resref, ext_to_type(t)
    if "." in resref:
        stem, ext = resref.rsplit(".", 1)
        rtype = ext_to_type(ext)
        if rtype is not None:
            return stem, rtype
    return resref, None


def _write_json(path: str, obj) -> None:
    p = Path(path).resolve()
    for parent in (p, *p.parents):
        if (parent / "chitin.key").exists() or (parent / "swkotor.exe").exists():
            raise SystemExit(f"refusing to write inside a game install: {p}")
    p.parent.mkdir(parents=True, exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="KotOR five-pass VFS resolver (read-only)")
    ap.add_argument("--game-dir", required=True)
    ap.add_argument("--active-module")
    ap.add_argument("--loaded-save")
    ap.add_argument("--quality", default="auto",
                    choices=["auto", "tpa", "tpb", "tpc", "all", "none"])
    ap.add_argument("--first-launch", action="store_true")
    ap.add_argument("--main-menu", action="store_true")
    ap.add_argument("--chargen", action="store_true")
    ap.add_argument("--no-texture-packs", action="store_true")
    ap.add_argument("--extra-rim", action="append", default=[])
    ap.add_argument("--extra-erf", action="append", default=[])
    ap.add_argument("--extra-dir", action="append", default=[])
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("mounts")
    for name in ("lookup", "trace"):
        sp = sub.add_parser(name)
        sp.add_argument("resref")
        sp.add_argument("type", nargs="?")
        sp.add_argument("--json")
        sp.add_argument("--hash", action="store_true")
    args = ap.parse_args(argv)

    vfs = Vfs(args.game_dir, active_module=args.active_module,
              loaded_save=args.loaded_save, quality=args.quality,
              first_launch=args.first_launch, main_menu=args.main_menu,
              chargen=args.chargen,
              include_texture_packs=not args.no_texture_packs,
              extra_rims=args.extra_rim, extra_erfs=args.extra_erf,
              extra_dirs=args.extra_dir)
    if args.cmd == "mounts":
        data = vfs.mounts()
        print(json.dumps(data, indent=2))
        return 0
    resref, rtype = _parse_query(args.resref, args.type)
    if rtype is None:
        print("a resource type (number or extension) is required", file=sys.stderr)
        return 2
    res = vfs.trace(resref, rtype)
    info = res.info(include_hash=args.hash)
    if args.json:
        _write_json(args.json, info)
    if args.cmd == "lookup":
        if not res.found:
            print(f"NOT FOUND {resref.lower()} type={rtype} ({type_label(rtype)})")
            print(f"tables searched: {res.tables_searched}")
            return 1
        w = res.winner
        print(f"FOUND {resref.lower()} type={rtype} ({type_label(rtype)})")
        print(f"  pass={w.pass_no} ({w.pass_name}) tier={w.tier}")
        print(f"  table={w.table.name} kind={w.table.kind} entry={w.entry.index}")
        print(f"  offset={w.entry.offset} size={w.entry.size}")
        print(f"  source={w.entry.source}")
        if w.duplicate_indices and len(w.duplicate_indices) > 1:
            print(f"  duplicate entry indices (first wins): {w.duplicate_indices}")
        if w.error:
            print(f"  error={w.error}")
        if args.hash and res.found:
            data = res.read_payload()
            if data is not None:
                print(f"  payload_sha256={hashlib.sha256(data).hexdigest()}")
        print(f"  tables searched: {res.tables_searched}; hits: {len(res.traces)}")
        for h in res.traces[1:]:
            print(f"  shadowed: pass={h.pass_no} table={h.table.name} "
                  f"entry={h.entry.index} size={h.entry.size}")
        return 0
    print(json.dumps(info, indent=2))
    return 0 if res.found else 1


if __name__ == "__main__":
    sys.exit(main())
