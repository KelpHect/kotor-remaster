#!/usr/bin/env python3
"""Read-only KotOR save-folder parser and structural writer skeleton (lane 05).

Layout implemented from docs/recon/02_TECHNICAL_NOTES.md section 7:

  <slot>/
    SAVEGAME.sav       ERF tagged MOD V1.0; one nested <module>.sav per module
    savenfo.res        NFO GFF
    PARTYTABLE.res     PT GFF (party, gold, journal)
    GLOBALVARS.res     GVT GFF (positional category tables)
    Screen.tga         screenshot (absent on autosave)
    pifo.ifo           player info (autosave; may also exist elsewhere)
    CORRUPT.res        empty failed-unpack sentinel

Each nested module ERF contains Module.ifo (IFO), Module.are (ARE; NWM modules
may omit it) and Module.git (GIT).  GFF parsing uses the verified project
parser ``tools/formats/gff.py``.  Embedded GFFs are written to short-lived
files under this lane's ``tmp/`` because that parser accepts paths, not bytes;
the save folder itself is opened read-only.

The writer skeleton (SaveWriter / SaveFolder.rewrite_to) re-serializes GFF
trees semantically and rebuilds ERF tables.  It is not byte-identical and has
not been accepted by the retail engine.  Output is refused inside a game
install (a directory containing chitin.key or swkotor.exe).

No real save exists in the reference install at the time of writing; the
synthetic fixtures are the only validated inputs.  See README/RESULTS.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import tempfile
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for _p in (REPO_ROOT / "tools" / "resources", REPO_ROOT / "tools" / "formats",
           REPO_ROOT / "tools" / "inventory"):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

import gff  # noqa: E402
import kotor_archive as ka  # noqa: E402
import kotor_gff_write as gw  # noqa: E402
from kotor_types import TYPE_INFO  # noqa: E402

TMP_ROOT = REPO_ROOT / "artifacts" / "tmp"
TMP_ROOT.mkdir(parents=True, exist_ok=True)

SIDECARS = {
    "savenfo.res": "nfo",
    "partytable.res": "party_table",
    "globalvars.res": "global_vars",
    "screen.tga": "screen",
    "pifo.ifo": "pifo",
    "savegame.sav": "outer",
    "corrupt.res": "corrupt",
}
MODULE_GFF_TYPES = {2014: "ifo", 2012: "are", 2023: "git"}
GFF_MAGIC = {b"NFO ", b"PT  ", b"GVT ", b"IFO ", b"ARE ", b"GIT ", b"FAC ",
             b"INV ", b"BIC ", b"UTC ", b"UTI "}
GIT_LISTS = {
    "creaturelist": "creatures",
    "doorlist": "doors",
    "placeablelist": "placeables",
    "triggerlist": "triggers",
    "encounterlist": "encounters",
    "storelist": "stores",
    "soundlist": "sounds",
    "waypointlist": "waypoints",
    "cameralist": "cameras",
    "list": "misc",
}
WINDOWS_TO_UNIX_EPOCH_100NS = 116_444_736_000_000_000


class SaveParseError(ValueError):
    pass


class SaveWriteError(ValueError):
    pass


# ---------------------------------------------------------------------------
# small helpers
# ---------------------------------------------------------------------------


def type_label(res_type: int) -> str:
    return TYPE_INFO.get(int(res_type), ("UNKNOWN",))[0]


def normalize_name(name: str) -> str:
    return name.split("\x00", 1)[0].lower()


def looks_like_gff(data: bytes) -> bool:
    if len(data) < 56:
        return False
    return data[4:8] in (b"V3.0", b"V3.1", b"V3.2", b"V3.3")


def looks_like_erf(data: bytes) -> bool:
    return len(data) >= 32 and data[:4] in (b"ERF ", b"MOD ", b"SAV ")


def gff_from_bytes(data: bytes, label: str = "embedded") -> "gff.Gff":
    """Parse embedded GFF bytes through the path-only project parser."""
    if not data:
        raise SaveParseError(f"{label}: empty GFF payload")
    fd, path = tempfile.mkstemp(prefix="gff-", suffix=".bin", dir=str(TMP_ROOT))
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        return gff.Gff(path)
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def gff_check(g: "gff.Gff") -> list[str]:
    try:
        return list(gff.check(g))
    except Exception as exc:  # malformed tables can make the checker fail too
        return [f"check raised: {exc}"]


def field_rows(g: "gff.Gff", si: int = 0):
    try:
        return g.fields_of(si)
    except Exception:
        return []


def find_field(g: "gff.Gff", label: str, si: int = 0):
    for f in field_rows(g, si):
        if f.label == label:
            return f
    return None


def get_value(g: "gff.Gff", label: str, default=None, si: int = 0):
    f = find_field(g, label, si)
    if f is None:
        return default
    try:
        return g.value(f)
    except Exception:
        return default


def get_str(g: "gff.Gff", label: str, default=None, si: int = 0):
    value = get_value(g, label, default, si)
    if value is None:
        return default
    if isinstance(value, bytes):
        return value.split(b"\x00", 1)[0].decode("latin-1", "replace")
    return value if isinstance(value, str) else str(value)


def get_int(g: "gff.Gff", label: str, default=None, si: int = 0):
    value = get_value(g, label, default, si)
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    return default


def list_count(g: "gff.Gff", label: str, si: int = 0):
    f = find_field(g, label, si)
    if f is None or f.type != 15:
        return None
    try:
        n, _idx = g.list_of(f)
        return n
    except Exception:
        return None


def list_indices(g: "gff.Gff", label: str, si: int = 0):
    f = find_field(g, label, si)
    if f is None or f.type != 15:
        return []
    try:
        _n, idx = g.list_of(f)
        return list(idx)
    except Exception:
        return []


def list_counts(g: "gff.Gff", si: int = 0) -> dict[str, int]:
    out = {}
    for f in field_rows(g, si):
        if f.type == 15:
            try:
                out[f.label] = g.list_of(f)[0]
            except Exception:
                out[f.label] = -1
    return out


def locstring_text(value) -> str | None:
    if not isinstance(value, dict):
        return None
    subs = value.get("substrings") or []
    for lang, text in subs:
        if lang in (0, 1) and text:
            return text
    if subs:
        return subs[0][1]
    return None


def json_safe(value):
    if isinstance(value, bytes):
        return {"bytes": len(value)}
    if isinstance(value, tuple):
        return list(value)
    if isinstance(value, dict):
        return {k: json_safe(v) for k, v in value.items()}
    if isinstance(value, list):
        return [json_safe(v) for v in value]
    if isinstance(value, float):
        return value
    return value


def redacted_text(text: str | None):
    if text is None:
        return None
    raw = text.encode("latin-1", "replace")
    return {"redacted": True, "text_len": len(text),
            "sha256_8": hashlib.sha256(raw).hexdigest()[:8]}


def text_out(text: str | None, redact: bool):
    return redacted_text(text) if redact else text


def gff_summary(g: "gff.Gff") -> dict:
    try:
        magic = g.magic.decode("latin-1", "replace")
        version = g.version.decode("latin-1", "replace")
        labels = [f.label for f in field_rows(g, 0)]
        return {
            "file_type": magic.strip(),
            "magic": magic,
            "version": version,
            "struct_count": g.struct_cnt,
            "field_count": g.field_cnt,
            "label_count": g.label_cnt,
            "labels": labels,
            "list_counts": list_counts(g, 0),
            "check_errors": gff_check(g),
        }
    except Exception as exc:
        return {"error": f"summary failed: {exc}", "check_errors": []}


def entry_summary(e: ka.ArchiveEntry) -> dict:
    return {"index": e.index, "resref": e.resref, "type": e.res_type,
            "kind": type_label(e.res_type), "id": e.res_id,
            "offset": e.offset, "size": e.size}


def tga_info(data: bytes) -> dict:
    info = {"size": len(data), "valid": False}
    if len(data) >= 18:
        id_len, cmap_type, image_type = data[0], data[1], data[2]
        width, height = struct.unpack_from("<HH", data, 12)
        bpp, descriptor = data[16], data[17]
        info.update({"id_length": id_len, "color_map_type": cmap_type,
                     "image_type": image_type, "width": width,
                     "height": height, "bits_per_pixel": bpp,
                     "descriptor": descriptor,
                     "valid": image_type in (1, 2, 3, 9, 10, 11) and width > 0 and height > 0})
    return info


# ---------------------------------------------------------------------------
# field models
# ---------------------------------------------------------------------------


def ifo_fields(g: "gff.Gff", redact: bool = False) -> dict:
    area = get_str(g, "Mod_Entry_Area", "")
    area_list = []
    for si in list_indices(g, "Mod_Area_list"):
        name = get_str(g, "Area_Name", None, si)
        if name:
            area_list.append(name)
    if not area and area_list:
        area = area_list[0]
    name_val = get_value(g, "Mod_Name")
    name_text = locstring_text(name_val) if isinstance(name_val, dict) else None
    return {
        "entry": {
            "area": area,
            "x": get_value(g, "Mod_Entry_X"),
            "y": get_value(g, "Mod_Entry_Y"),
            "z": get_value(g, "Mod_Entry_Z"),
            "dir_x": get_value(g, "Mod_Entry_Dir_X"),
            "dir_y": get_value(g, "Mod_Entry_Dir_Y"),
        },
        "tag": get_str(g, "Mod_Tag", ""),
        "name": text_out(name_text, redact),
        "area_list": area_list,
        "area_list_truncated": False,
        "is_save_game": get_int(g, "Mod_IsSaveGame"),
        "version": get_int(g, "Mod_Version"),
        "start": {
            "hour": get_int(g, "Mod_StartHour"),
            "day": get_int(g, "Mod_StartDay"),
            "month": get_int(g, "Mod_StartMonth"),
            "year": get_int(g, "Mod_StartYear"),
            "minutes_per_hour": get_int(g, "Mod_MinPerHour"),
        },
        "xp_scale": get_int(g, "Mod_XPScale"),
        "list_counts": list_counts(g, 0),
    }


def are_fields(g: "gff.Gff", redact: bool = False) -> dict:
    name_val = get_value(g, "Name")
    name_text = locstring_text(name_val) if isinstance(name_val, dict) else None
    return {
        "tag": get_str(g, "Tag", ""),
        "name": text_out(name_text, redact),
        "flags": get_int(g, "Flags"),
        "camera_style": get_int(g, "CameraStyle"),
        "load_screen_id": get_int(g, "LoadScreenID"),
        "rooms": list_count(g, "Rooms"),
        "list_counts": list_counts(g, 0),
    }


def git_fields(g: "gff.Gff") -> dict:
    raw_counts = list_counts(g, 0)
    counts = {}
    for label, n in raw_counts.items():
        key = re.sub(r"\s+", "", label).lower()
        counts[GIT_LISTS.get(key, key)] = n
    object_total = sum(v for k, v in counts.items() if k != "misc" and v >= 0)
    return {
        "use_templates": get_int(g, "UseTemplates"),
        "object_counts": counts,
        "total_instances": object_total,
        "list_counts": raw_counts,
    }


def party_fields(g: "gff.Gff", redact: bool = False) -> dict:
    members = []
    for si in list_indices(g, "PT_MEMBERS"):
        members.append({
            "leader": bool(get_int(g, "PT_IS_LEADER", 0, si)),
            "member_id": get_int(g, "PT_MEMBER_ID", None, si),
        })
    avail = 0
    selectable = 0
    for si in list_indices(g, "PT_AVAIL_NPCS"):
        if get_int(g, "PT_NPC_AVAIL", 0, si):
            avail += 1
        if get_int(g, "PT_NPC_SELECT", 0, si):
            selectable += 1
    influence = list_indices(g, "PT_INFLUENCE")
    journal = []
    states: Counter = Counter()
    for si in list_indices(g, "JNL_Entries"):
        plot = get_str(g, "JNL_PlotID", "", si)
        state = get_int(g, "JNL_State", None, si)
        states[str(state)] += 1
        journal.append({
            "plot_id": text_out(plot, redact),
            "state": state,
            "date": get_int(g, "JNL_Date", None, si),
            "time": get_int(g, "JNL_Time", None, si),
        })
    return {
        "gold": get_int(g, "PT_GOLD"),
        "member_count": len(members),
        "members": members,
        "available_npc_count": avail,
        "selectable_npc_count": selectable,
        "influence_count": len(influence),
        "journal_count": len(journal),
        "journal_states": dict(sorted(states.items())),
        "journal_sample": journal[:20],
        "journal_truncated": len(journal) > 20,
        "chemical_count": get_int(g, "PT_ITEM_CHEMICAL"),
        "component_count": get_int(g, "PT_ITEM_COMPONEN"),
        "list_counts": list_counts(g, 0),
    }


def _cat_names(g: "gff.Gff", label: str) -> list[str]:
    out = []
    for si in list_indices(g, label):
        name = get_str(g, "Name", "", si)
        out.append(name)
    return out


def _binary(g: "gff.Gff", label: str) -> bytes:
    value = get_value(g, label)
    if isinstance(value, (bytes, bytearray)):
        return bytes(value)
    return b""


def _read_bool(bits: bytes, index: int) -> bool:
    byte_index = index // 8
    if byte_index < 0 or byte_index >= len(bits):
        return False
    return bool((bits[byte_index] >> (7 - (index % 8))) & 1)


def global_fields(g: "gff.Gff", redact: bool = False) -> dict:
    number_names = _cat_names(g, "CatNumber")
    boolean_names = _cat_names(g, "CatBoolean")
    string_names = _cat_names(g, "CatString")
    location_names = _cat_names(g, "CatLocation")
    number_bytes = _binary(g, "ValNumber")
    boolean_bytes = _binary(g, "ValBoolean")
    location_bytes = _binary(g, "ValLocation")
    string_values = []
    for si in list_indices(g, "ValString"):
        string_values.append(get_str(g, "String", "", si))
    numbers = [(name, number_bytes[i] if i < len(number_bytes) else 0)
               for i, name in enumerate(number_names)]
    booleans = [(name, _read_bool(boolean_bytes, i))
                for i, name in enumerate(boolean_names)]
    locations = []
    for i, name in enumerate(location_names):
        base = i * 24
        if base + 24 <= len(location_bytes):
            vals = struct.unpack_from("<6f", location_bytes, base)
            locations.append((name, list(vals)))
        else:
            locations.append((name, None))
    strings = [(name, string_values[i] if i < len(string_values) else None)
               for i, name in enumerate(string_names)]
    checks = []
    if len(number_bytes) < len(number_names):
        checks.append(f"ValNumber has {len(number_bytes)} bytes for {len(number_names)} names")
    expected_bool = (len(boolean_names) >> 3) + 1 if boolean_names else 0
    if boolean_names and len(boolean_bytes) < expected_bool:
        checks.append(f"ValBoolean has {len(boolean_bytes)} bytes, expected {expected_bool}")
    if len(string_values) != len(string_names):
        checks.append(f"ValString has {len(string_values)} values for {len(string_names)} names")
    if location_bytes and len(location_bytes) % 24:
        checks.append(f"ValLocation length {len(location_bytes)} is not a multiple of 24")
    if len(locations) != len(location_names):
        checks.append(f"ValLocation has {len(location_bytes)//24} locations for {len(location_names)} names")
    return {
        "category_counts": {
            "number": len(number_names),
            "boolean": len(boolean_names),
            "string": len(string_names),
            "location": len(location_names),
        },
        "val_number_bytes": len(number_bytes),
        "val_boolean_bytes": len(boolean_bytes),
        "val_location_bytes": len(location_bytes),
        "val_string_count": len(string_values),
        "boolean_true_count": sum(1 for _n, v in booleans if v),
        "numbers": [{"name": text_out(n, redact), "value": v} for n, v in numbers],
        "booleans": [{"name": text_out(n, redact), "value": v} for n, v in booleans],
        "strings": [{"name": text_out(n, redact), "value": text_out(v, redact)}
                    for n, v in strings],
        "locations": [{"name": text_out(n, redact), "value": v} for n, v in locations],
        "checks": checks,
        "checks_structure": "ValBoolean is MSB-first in each byte (prior-art behavior)",
    }


def inventory_fields(g: "gff.Gff") -> dict:
    raw_counts = list_counts(g, 0)
    item_label = None
    for label in raw_counts:
        if "item" in label.lower():
            item_label = label
            break
    if item_label is None and raw_counts:
        item_label = next(iter(raw_counts))
    items = []
    for si in (list_indices(g, item_label) if item_label else []):
        res = get_str(g, "InventoryRes", None, si)
        if res is None:
            res = get_str(g, "InventoryResRef", None, si)
        items.append({
            "resref": res,
            "x": get_int(g, "Repos_PosX", None, si),
            "y": get_int(g, "Repos_PosY", None, si),
        })
    return {"item_list_label": item_label, "item_count": raw_counts.get(item_label),
            "items": items, "list_counts": raw_counts}


def pc_fields(g: "gff.Gff", redact: bool = False) -> dict:
    first = get_str(g, "FirstName", None)
    last = get_str(g, "LastName", None)
    return {
        "tag": get_str(g, "Tag", ""),
        "first_name": text_out(first, redact),
        "last_name": text_out(last, redact),
        "list_counts": list_counts(g, 0),
        "inventory_count": list_count(g, "InventoryList"),
        "equipment_count": list_count(g, "Equip_ItemList"),
    }


# ---------------------------------------------------------------------------
# save model
# ---------------------------------------------------------------------------


class SaveFolder:
    def __init__(self, path: str | os.PathLike):
        p = Path(path)
        if p.is_file():
            if p.name.lower() != "savegame.sav":
                raise SaveParseError(f"{p}: expected a save folder or SAVEGAME.sav")
            p = p.parent
        if not p.is_dir():
            raise SaveParseError(f"{p}: not a directory")
        self.path = p
        self.files = {}
        for child in sorted(p.iterdir(), key=lambda x: x.name.lower()):
            if child.is_file():
                self.files.setdefault(child.name.lower(), child)
        self.outer_archive: ka.Archive | None = None
        self.module_archives: list[ka.Archive] = []
        self._last_model: dict | None = None

    def file(self, name: str) -> Path | None:
        return self.files.get(name.lower())

    def _gff_file(self, name: str, expected_magic: str,
                  redact: bool) -> dict:
        path = self.file(name)
        if path is None:
            return {"present": False, "file": name, "size": 0,
                    "check_errors": [], "checks": [f"missing {name}"]}
        try:
            data = path.read_bytes()
        except OSError as exc:
            return {"present": True, "file": path.name, "size": 0,
                    "check_errors": [str(exc)], "checks": []}
        summary = {"present": True, "file": path.name, "size": len(data),
                   "check_errors": [], "checks": []}
        if not data:
            summary["check_errors"].append(f"{path.name}: empty file")
            return summary
        try:
            g = gff_from_bytes(data, path.name)
        except Exception as exc:
            summary["check_errors"].append(f"{path.name}: {exc}")
            return summary
        summary.update(gff_summary(g))
        summary["expected_file_type"] = expected_magic.strip()
        summary["magic_ok"] = summary.get("file_type") == expected_magic.strip()
        return summary

    def _parse_sidecars(self, model: dict, redact: bool) -> None:
        nfo = self._gff_file("savenfo.res", "NFO ", redact)
        if nfo.get("present") and "labels" in nfo:
            try:
                g = gff_from_bytes(self.file("savenfo.res").read_bytes(), "savenfo.res")
                nfo.update(self._nfo_fields(g, redact))
            except Exception as exc:
                nfo["check_errors"].append(f"selected fields: {exc}")
        model["nfo"] = nfo

        pt = self._gff_file("PARTYTABLE.res", "PT  ", redact)
        if pt.get("present") and "labels" in pt:
            try:
                g = gff_from_bytes(self.file("PARTYTABLE.res").read_bytes(), "PARTYTABLE.res")
                pt.update(party_fields(g, redact))
            except Exception as exc:
                pt["check_errors"].append(f"selected fields: {exc}")
        model["party_table"] = pt

        gv = self._gff_file("GLOBALVARS.res", "GVT ", redact)
        if gv.get("present") and "labels" in gv:
            try:
                g = gff_from_bytes(self.file("GLOBALVARS.res").read_bytes(), "GLOBALVARS.res")
                gv.update(global_fields(g, redact))
            except Exception as exc:
                gv["check_errors"].append(f"selected fields: {exc}")
        model["global_vars"] = gv

        pifo = self._gff_file("pifo.ifo", "IFO ", redact)
        if pifo.get("present") and "labels" in pifo:
            try:
                g = gff_from_bytes(self.file("pifo.ifo").read_bytes(), "pifo.ifo")
                pifo["tag"] = get_str(g, "Mod_Tag", None)
                pifo["area_list"] = [
                    get_str(g, "Area_Name", None, si)
                    for si in list_indices(g, "Mod_Area_list")
                ]
            except Exception as exc:
                pifo["check_errors"].append(f"selected fields: {exc}")
        model["pifo"] = pifo

        screen = self.file("Screen.tga")
        if screen is not None:
            try:
                data = screen.read_bytes()
                model["screen"] = {"present": True, "file": screen.name,
                                   "size": len(data), "tga": tga_info(data)}
            except OSError as exc:
                model["screen"] = {"present": True, "file": screen.name,
                                   "size": 0, "error": str(exc)}
        else:
            model["screen"] = {"present": False, "file": "Screen.tga", "size": 0}

        corrupt = self.file("CORRUPT.res")
        model["corrupt"] = ({"present": True, "file": corrupt.name,
                             "size": corrupt.stat().st_size} if corrupt
                            else {"present": False})

        model["sidecar_files"] = {
            key: ({"present": True, "file": p.name, "size": p.stat().st_size}
                  if (p := self.file(name)) else {"present": False})
            for name, key in SIDECARS.items() if key != "outer"
        }

    def _nfo_fields(self, g: "gff.Gff", redact: bool) -> dict:
        timestamp = get_int(g, "TIMESTAMP")
        unix = None
        if isinstance(timestamp, int) and timestamp > WINDOWS_TO_UNIX_EPOCH_100NS:
            unix = (timestamp - WINDOWS_TO_UNIX_EPOCH_100NS) / 10_000_000.0
        return {
            "area_name": text_out(get_str(g, "AREANAME", None), redact),
            "last_module": text_out(get_str(g, "LASTMODULE", None), redact),
            "save_game_name": text_out(get_str(g, "SAVEGAMENAME", None), redact),
            "pc_name": text_out(get_str(g, "PCNAME", None), redact),
            "time_played": get_int(g, "TIMEPLAYED"),
            "timestamp_filetime": timestamp,
            "timestamp_unix": unix,
            "cheat_used": bool(get_int(g, "CHEATUSED", 0)),
            "gameplay_hint": get_int(g, "GAMEPLAYHINT"),
            "story_hint": get_int(g, "STORYHINT"),
            "portraits": [get_str(g, f"PORTRAIT{i}", None) for i in range(3)],
            "screenshot": get_str(g, "SCREENSHOT", None),
            "live": [text_out(get_str(g, f"LIVE{i}", None), redact) for i in range(1, 7)],
        }

    def _parse_module(self, entry: ka.ArchiveEntry, data: bytes,
                      redact: bool) -> dict:
        m = {
            "resref": entry.resref,
            "type": entry.res_type,
            "size": len(data),
            "nested_magic": data[:4].decode("latin-1", "replace"),
            "nested_version": data[4:8].decode("latin-1", "replace"),
            "entry_count": None,
            "entries": [],
            "ifo": None,
            "are": None,
            "git": None,
            "others": [],
            "errors": [],
        }
        try:
            nested = ka.read_erf_bytes(data, f"{entry.resref}.sav")
        except Exception as exc:
            m["errors"].append(f"nested ERF: {exc}")
            return m
        self.module_archives.append(nested)
        m["entry_count"] = len(nested.entries)
        m["entries"] = [entry_summary(e) for e in nested.entries]
        for ne in nested.entries:
            try:
                ndata = nested.read_entry(ne)
            except Exception as exc:
                m["errors"].append(f"{ne.resref}: {exc}")
                continue
            kind = MODULE_GFF_TYPES.get(ne.res_type)
            if kind is None:
                m["others"].append(entry_summary(ne))
                continue
            try:
                g = gff_from_bytes(ndata, f"{entry.resref}/{ne.resref}.{kind}")
            except Exception as exc:
                m["errors"].append(f"{kind}: {exc}")
                continue
            summary = gff_summary(g)
            if kind == "ifo":
                summary.update(ifo_fields(g, redact))
                if m["ifo"] is not None:
                    m["errors"].append("duplicate IFO resource")
                    continue
                m["ifo"] = summary
            elif kind == "are":
                summary.update(are_fields(g, redact))
                if m["are"] is not None:
                    m["errors"].append("duplicate ARE resource")
                    continue
                m["are"] = summary
            elif kind == "git":
                summary.update(git_fields(g))
                if m["git"] is not None:
                    m["errors"].append("duplicate GIT resource")
                    continue
                m["git"] = summary
        return m

    def _parse_outer(self, model: dict, redact: bool) -> None:
        outer_path = self.file("SAVEGAME.sav")
        outer = {"present": outer_path is not None,
                 "file": outer_path.name if outer_path else "SAVEGAME.sav",
                 "size": outer_path.stat().st_size if outer_path else 0,
                 "errors": [], "entries": [], "duplicates": []}
        model["outer"] = outer
        if outer_path is None:
            outer["errors"].append("SAVEGAME.sav missing")
            model["modules"] = []
            return
        try:
            self.outer_archive = ka.read_erf(outer_path)
        except Exception as exc:
            outer["errors"].append(str(exc))
            model["modules"] = []
            return
        a = self.outer_archive
        outer.update({
            "magic": a.magic, "version": a.version,
            "entry_count": len(a.entries),
            "entries": [entry_summary(e) for e in a.entries],
            "duplicates": [{"resref": k[0], "type": k[1], "indices": v}
                           for k, v in a.duplicates().items()],
        })
        modules = []
        inventory = None
        repute = None
        pcs = []
        avail = []
        outer_fallback_party = None
        outer_fallback_globals = None
        for e in a.entries:
            try:
                data = a.read_entry(e)
            except Exception as exc:
                outer["errors"].append(f"{e.resref}: {exc}")
                continue
            lname = normalize_name(e.resref)
            is_module = e.res_type == 2057 or (lname.endswith(".sav") and looks_like_erf(data))
            if is_module:
                modules.append(self._parse_module(e, data, redact))
                continue
            if lname == "inventory" and looks_like_gff(data):
                try:
                    g = gff_from_bytes(data, "INVENTORY")
                    inventory = {"entry": entry_summary(e), **gff_summary(g),
                                 **inventory_fields(g)}
                except Exception as exc:
                    inventory = {"entry": entry_summary(e), "error": str(exc)}
                continue
            if (e.res_type == 2038 or lname == "repute") and looks_like_gff(data):
                try:
                    g = gff_from_bytes(data, "REPUTE")
                    repute = {"entry": entry_summary(e), **gff_summary(g),
                              "list_counts": list_counts(g, 0)}
                except Exception as exc:
                    repute = {"entry": entry_summary(e), "error": str(exc)}
                continue
            if (e.res_type == 2015 or lname.startswith("pc")) and looks_like_gff(data):
                try:
                    g = gff_from_bytes(data, "PC")
                    pcs.append({"entry": entry_summary(e), **gff_summary(g),
                                **pc_fields(g, redact)})
                except Exception as exc:
                    pcs.append({"entry": entry_summary(e), "error": str(exc)})
                continue
            if e.res_type == 2027 and lname.startswith("availnpc") and looks_like_gff(data):
                try:
                    g = gff_from_bytes(data, "AVAILNPC")
                    avail.append({"entry": entry_summary(e), **gff_summary(g)})
                except Exception as exc:
                    avail.append({"entry": entry_summary(e), "error": str(exc)})
                continue
            if lname == "partytable" and looks_like_gff(data):
                outer_fallback_party = data
            elif lname == "globalvars" and looks_like_gff(data):
                outer_fallback_globals = data
        model["modules"] = modules
        model["inventory"] = inventory or {"present": False}
        model["repute"] = repute or {"present": False}
        model["pcs"] = pcs
        model["avail_npcs"] = avail

        # Fallbacks only matter for unusual saves that embed sidecar payloads.
        if not model["party_table"].get("present") and outer_fallback_party:
            try:
                g = gff_from_bytes(outer_fallback_party, "outer PARTYTABLE")
                model["party_table"].update({"present": True, "source": "SAVEGAME.sav",
                                             **gff_summary(g), **party_fields(g, redact)})
            except Exception as exc:
                model["party_table"]["check_errors"].append(str(exc))
        if not model["global_vars"].get("present") and outer_fallback_globals:
            try:
                g = gff_from_bytes(outer_fallback_globals, "outer GLOBALVARS")
                model["global_vars"].update({"present": True, "source": "SAVEGAME.sav",
                                             **gff_summary(g), **global_fields(g, redact)})
            except Exception as exc:
                model["global_vars"]["check_errors"].append(str(exc))

    def parse(self, redact: bool = False) -> dict:
        if self._last_model is not None:
            return self._last_model
        folder = self.path.name
        match = re.match(r"^(\d{6})\s*-\s*(.+)$", folder)
        slot = int(match.group(1)) if match else None
        label = match.group(2).strip() if match else folder
        upper = label.upper()
        if upper == "AUTOSAVE" or slot == 1:
            kind = "autosave"
        elif upper == "QUICKSAVE" or slot == 0:
            kind = "quicksave"
        elif slot is not None:
            kind = "manual"
        else:
            kind = "unknown"
        model = {
            "folder": str(self.path.resolve()),
            "folder_name": folder,
            "slot": slot,
            "label": label,
            "kind": kind,
            "autosave": kind == "autosave",
            "files": [{"name": p.name, "size": p.stat().st_size,
                       "canonical": next((k for k in SIDECARS
                                          if k.lower() == p.name.lower()), None)}
                      for p in sorted(self.files.values(), key=lambda x: x.name.lower())],
            "validation": {"errors": [], "warnings": [], "infos": []},
        }
        self._parse_sidecars(model, redact)
        self._parse_outer(model, redact)
        model["validation"] = validate_model(model)
        self._last_model = model
        return model

    # ------------------------------------------------------------------
    # writer skeleton
    # ------------------------------------------------------------------

    def rewrite_to(self, dst: str | os.PathLike, overwrite: bool = False) -> dict:
        """Semantically re-serialize GFF/ERF content into a new save folder.

        Non-GFF resources (screenshots, unknown files, FAC/INV payloads) are
        copied byte-for-byte.  GFF payloads are decoded and re-encoded by the
        lane writer.  The result is parsed again and returned.
        """
        model = self.parse()
        target = Path(dst)
        ensure_writable_target(target, source=self.path)
        if target.exists() and not overwrite:
            raise SaveWriteError(f"{target}: exists (use overwrite)")
        target.mkdir(parents=True, exist_ok=True)

        # Sidecars and loose files: rewrite GFF payloads, copy everything else.
        for p in sorted(self.files.values(), key=lambda x: x.name.lower()):
            data = p.read_bytes()
            out = data
            if looks_like_gff(data):
                try:
                    g = gff_from_bytes(data, p.name)
                    out = gw.gff_bytes(g.magic, gw.from_gff(g), g.version)
                except Exception:
                    out = data
            (target / p.name).write_bytes(out)

        if self.outer_archive is None:
            raise SaveWriteError("no outer SAVEGAME.sav to rewrite")
        outer_entries = []
        for e in self.outer_archive.entries:
            data = self.outer_archive.read_entry(e)
            if e.res_type == 2057 and looks_like_erf(data):
                nested = ka.read_erf_bytes(data, f"{e.resref}.sav")
                nested_entries = []
                for ne in nested.entries:
                    ndata = nested.read_entry(ne)
                    if looks_like_gff(ndata):
                        try:
                            g = gff_from_bytes(ndata, f"{e.resref}/{ne.resref}")
                            ndata = gw.gff_bytes(g.magic, gw.from_gff(g), g.version)
                        except Exception:
                            pass
                    nested_entries.append((ne.resref, ne.res_type, ndata))
                data = ka.erf_bytes(nested_entries,
                                    magic=nested.magic.encode("latin-1"),
                                    version=nested.version.encode("latin-1"))
            outer_entries.append((e.resref, e.res_type, data))
        blob = ka.erf_bytes(outer_entries,
                            magic=self.outer_archive.magic.encode("latin-1"),
                            version=self.outer_archive.version.encode("latin-1"))
        (target / "SAVEGAME.sav").write_bytes(blob)
        return SaveFolder(target).parse()


# ---------------------------------------------------------------------------
# validation
# ---------------------------------------------------------------------------


def validate_model(model: dict) -> dict:
    errors: list[dict] = []
    warnings: list[dict] = []
    infos: list[dict] = []

    def add(bucket, code, message, where=None):
        item = {"code": code, "message": message}
        if where:
            item["where"] = where
        bucket.append(item)

    if model.get("slot") is None:
        add(warnings, "folder.name", "folder does not match 'NNNNNN - NAME'")

    outer = model.get("outer", {})
    if not outer.get("present"):
        add(errors, "outer.missing", "SAVEGAME.sav is missing")
    else:
        for msg in outer.get("errors", []):
            add(errors, "outer.parse", msg)
        if outer.get("magic") not in ("MOD ", "ERF ", "SAV "):
            add(errors, "outer.magic", f"unexpected outer magic {outer.get('magic')!r}")
        elif outer.get("magic") != "MOD ":
            add(warnings, "outer.tag", f"outer archive is {outer.get('magic')!r}, expected 'MOD '")
        if outer.get("duplicates"):
            add(warnings, "outer.duplicates",
                f"{len(outer['duplicates'])} duplicate (resref,type) keys in outer ERF")

    nfo = model.get("nfo", {})
    if not nfo.get("present"):
        add(errors, "nfo.missing", "savenfo.res is missing")
    else:
        for msg in nfo.get("check_errors", []):
            add(errors, "nfo.gff", msg)
        if nfo.get("labels") and not nfo.get("magic_ok"):
            add(warnings, "nfo.magic", f"savenfo.res type {nfo.get('file_type')!r}, expected NFO")
        if nfo.get("last_module") is None:
            add(warnings, "nfo.last_module", "LASTMODULE absent")
        if nfo.get("save_game_name") is None:
            add(warnings, "nfo.name", "SAVEGAMENAME absent")
        if isinstance(nfo.get("last_module"), str) and not nfo["last_module"]:
            add(warnings, "nfo.last_module", "LASTMODULE empty")

    pt = model.get("party_table", {})
    if not pt.get("present"):
        add(errors, "party_table.missing", "PARTYTABLE.res is missing")
    else:
        for msg in pt.get("check_errors", []):
            add(errors, "party_table.gff", msg)
        if pt.get("labels") and not pt.get("magic_ok"):
            add(warnings, "party_table.magic", f"PARTYTABLE type {pt.get('file_type')!r}, expected PT")
        if pt.get("gold") is None:
            add(warnings, "party_table.gold", "PT_GOLD absent")
        leaders = sum(1 for m in pt.get("members", []) if m.get("leader"))
        if pt.get("members") and leaders != 1:
            add(warnings, "party_table.leader", f"{leaders} leaders flagged in PT_MEMBERS")
        for m in pt.get("members", []):
            if isinstance(m.get("member_id"), int) and m["member_id"] < 0:
                add(warnings, "party_table.member_id", "negative PT_MEMBER_ID")

    gv = model.get("global_vars", {})
    if not gv.get("present"):
        add(errors, "global_vars.missing", "GLOBALVARS.res is missing")
    else:
        for msg in gv.get("check_errors", []):
            add(errors, "global_vars.gff", msg)
        if gv.get("labels") and not gv.get("magic_ok"):
            add(warnings, "global_vars.magic", f"GLOBALVARS type {gv.get('file_type')!r}, expected GVT")
        for msg in gv.get("checks", []):
            add(warnings, "global_vars.layout", msg)

    autosave = model.get("autosave", False)
    screen = model.get("screen", {})
    pifo = model.get("pifo", {})
    if autosave:
        if screen.get("present"):
            add(warnings, "autosave.screen", "autosave has Screen.tga")
        if not pifo.get("present"):
            add(warnings, "autosave.pifo", "autosave lacks pifo.ifo")
    else:
        if not screen.get("present"):
            add(warnings, "manual.screen", "non-autosave save lacks Screen.tga")
        if screen.get("present") and not (screen.get("tga") or {}).get("valid"):
            add(warnings, "screen.tga", "Screen.tga is shorter than a valid TGA header")
    if model.get("corrupt", {}).get("present"):
        add(errors, "corrupt.sentinel", "CORRUPT.res present (failed unpack)")

    modules = model.get("modules", [])
    if not modules:
        add(errors, "modules.none", "outer save contains no type-2057 module ERFs")
    seen_modules = Counter()
    for m in modules:
        where = m.get("resref")
        seen_modules[m.get("resref", "").lower()] += 1
        for msg in m.get("errors", []):
            add(errors, "module.parse", msg, where)
        if not m.get("nested_magic", "").strip():
            add(errors, "module.magic", "nested archive has no magic", where)
        if m.get("ifo") is None:
            add(errors, "module.ifo_missing", "module has no IFO", where)
        else:
            for msg in m["ifo"].get("check_errors", []):
                add(errors, "module.ifo_gff", msg, where)
            entry = m["ifo"].get("entry", {})
            if not entry.get("area"):
                add(errors, "module.entry_area", "Mod_Entry_Area absent/empty", where)
            if not m["ifo"].get("tag"):
                add(warnings, "module.tag", "Mod_Tag absent/empty", where)
            if m["ifo"].get("is_save_game") == 0:
                add(warnings, "module.save_flag", "Mod_IsSaveGame is 0 in saved module", where)
            area_list = m["ifo"].get("area_list") or []
            if area_list and entry.get("area") and entry["area"].lower() not in {a.lower() for a in area_list}:
                add(warnings, "module.area_list", "entry area is not in Mod_Area_list", where)
        if m.get("git") is None:
            add(errors, "module.git_missing", "module has no GIT", where)
        else:
            for msg in m["git"].get("check_errors", []):
                add(errors, "module.git_gff", msg, where)
            if m["git"].get("use_templates") == 1:
                add(warnings, "module.use_templates",
                    "UseTemplates=1 in saved GIT; saved objects should be flattened", where)
        if m.get("are") is None:
            add(infos, "module.are_missing", "module has no ARE (NWM-style or unusual)", where)
        else:
            for msg in m["are"].get("check_errors", []):
                add(errors, "module.are_gff", msg, where)
        if m.get("others"):
            add(infos, "module.extra", f"{len(m['others'])} extra module resources", where)
    for name, count in seen_modules.items():
        if count > 1:
            add(warnings, "module.duplicate", f"module {name!r} appears {count} times")

    last_module = nfo.get("last_module")
    if isinstance(last_module, str) and last_module and modules:
        known = {m.get("resref", "").lower() for m in modules}
        if last_module.lower() not in known:
            add(warnings, "nfo.last_module_match",
                f"LASTMODULE {last_module!r} has no matching nested module")

    return {"errors": errors, "warnings": warnings, "infos": infos}


# ---------------------------------------------------------------------------
# writer skeleton
# ---------------------------------------------------------------------------


def ensure_writable_target(path: str | os.PathLike,
                           source: str | os.PathLike | None = None) -> None:
    """Refuse writes inside a game install or overlapping the source tree."""
    target = Path(path).resolve()
    for parent in (target, *target.parents):
        if (parent / "chitin.key").exists() or (parent / "swkotor.exe").exists():
            raise SaveWriteError(f"{target}: refusing to write inside a game install")
    if source is not None:
        src = Path(source).resolve()
        if target == src or src in target.parents or target in src.parents:
            raise SaveWriteError(f"{target}: output overlaps source {src}")


@dataclass
class SaveWriter:
    """Build a structurally valid save folder from GFF trees.

    This is the writer skeleton used for fixtures.  It writes canonical
    sidecar names and one nested MOD ERF per module.  It does not claim the
    retail engine will accept the result.
    """

    out_dir: str | os.PathLike
    modules: list[tuple[str, bytes]] = field(default_factory=list)
    outer_entries: list[tuple[str, int, bytes]] = field(default_factory=list)
    sidecars: dict[str, bytes] = field(default_factory=dict)

    def add_module(self, resref: str, ifo: gw.GffStruct,
                   git: gw.GffStruct, are: gw.GffStruct | None = None,
                   extras: list[tuple[str, int, bytes]] | None = None) -> "SaveWriter":
        entries = [("module", 2014, gw.gff_bytes(b"IFO ", ifo)),
                   ("module", 2023, gw.gff_bytes(b"GIT ", git))]
        if are is not None:
            entries.insert(1, ("module", 2012, gw.gff_bytes(b"ARE ", are)))
        for e in extras or []:
            entries.append(e)
        self.modules.append((resref, ka.erf_bytes(entries, magic=b"MOD ")))
        return self

    def add_outer_gff(self, resref: str, res_type: int, file_type: bytes,
                      root: gw.GffStruct) -> "SaveWriter":
        self.outer_entries.append((resref, res_type, gw.gff_bytes(file_type, root)))
        return self

    def add_outer_bytes(self, resref: str, res_type: int, data: bytes) -> "SaveWriter":
        self.outer_entries.append((resref, res_type, data))
        return self

    def add_sidecar_gff(self, filename: str, file_type: bytes,
                        root: gw.GffStruct) -> "SaveWriter":
        self.sidecars[filename] = gw.gff_bytes(file_type, root)
        return self

    def add_sidecar_bytes(self, filename: str, data: bytes) -> "SaveWriter":
        self.sidecars[filename] = data
        return self

    def write(self, overwrite: bool = False) -> str:
        target = Path(self.out_dir)
        ensure_writable_target(target)
        if target.exists() and not overwrite:
            if any(target.iterdir()):
                raise SaveWriteError(f"{target}: exists and is not empty")
        target.mkdir(parents=True, exist_ok=True)
        if not self.modules:
            raise SaveWriteError("save writer requires at least one module")
        entries = [(name, 2057, blob) for name, blob in self.modules]
        entries.extend(self.outer_entries)
        outer = ka.erf_bytes(entries, magic=b"MOD ")
        (target / "SAVEGAME.sav").write_bytes(outer)
        for name, data in self.sidecars.items():
            (target / name).write_bytes(data)
        return str(target)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _load_json_arg(path: str | None):
    if not path:
        return None
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _write_json(path: str, obj) -> None:
    ensure_writable_target(path)
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, sort_keys=False)


def _summary(model: dict) -> str:
    v = model["validation"]
    lines = [
        f"folder: {model['folder_name']} (slot={model['slot']}, kind={model['kind']})",
        f"files: {len(model['files'])}; outer={model['outer'].get('entry_count')} entries",
        f"modules: {len(model.get('modules', []))}",
    ]
    for m in model.get("modules", []):
        ifo = m.get("ifo") or {}
        git = m.get("git") or {}
        entry = ifo.get("entry", {})
        lines.append(
            f"  {m.get('resref')}: ifo={bool(m.get('ifo'))} are={bool(m.get('are'))} "
            f"git={bool(m.get('git'))} area={entry.get('area')!r} "
            f"objects={git.get('total_instances')}"
        )
    pt = model.get("party_table", {})
    lines.append(f"party: present={pt.get('present')} gold={pt.get('gold')} "
                 f"members={pt.get('member_count')} journal={pt.get('journal_count')}")
    gv = model.get("global_vars", {})
    lines.append(f"globals: present={gv.get('present')} cats={gv.get('category_counts')}")
    nfo = model.get("nfo", {})
    lines.append(f"nfo: present={nfo.get('present')} last_module={nfo.get('last_module')!r}")
    lines.append(f"validation: {len(v['errors'])} errors, {len(v['warnings'])} warnings, "
                 f"{len(v['infos'])} infos")
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="KotOR save-folder structural parser")
    sub = ap.add_subparsers(dest="cmd", required=True)

    ls = sub.add_parser("list", help="list save folders under a Saves directory")
    ls.add_argument("--saves-dir", required=True)
    ls.add_argument("--json")

    pp = sub.add_parser("parse", help="parse one save folder")
    pp.add_argument("folder")
    pp.add_argument("--json")
    pp.add_argument("--redact", action="store_true")
    pp.add_argument("--quiet", action="store_true")

    vv = sub.add_parser("validate", help="parse and print validation")
    vv.add_argument("folder")
    vv.add_argument("--json")
    vv.add_argument("--redact", action="store_true")

    rr = sub.add_parser("rewrite", help="semantically rewrite a save folder")
    rr.add_argument("src")
    rr.add_argument("dst")
    rr.add_argument("--overwrite", action="store_true")

    args = ap.parse_args(argv)
    if args.cmd == "list":
        root = Path(args.saves_dir)
        rows = []
        if root.is_dir():
            for p in sorted(root.iterdir(), key=lambda x: x.name.lower()):
                if p.is_dir():
                    row = {"folder": p.name}
                    sav = next((c for c in p.iterdir() if c.name.lower() == "savegame.sav"), None)
                    row["has_savegame"] = sav is not None
                    row["savegame_size"] = sav.stat().st_size if sav else 0
                    m = re.match(r"^(\d{6})\s*-\s*(.+)$", p.name)
                    row["slot"] = int(m.group(1)) if m else None
                    row["label"] = m.group(2) if m else p.name
                    rows.append(row)
        if args.json:
            _write_json(args.json, {"saves_dir": str(root), "folders": rows})
        for row in rows:
            print(f"{row['folder']}  savegame={row['has_savegame']} size={row['savegame_size']}")
        if not rows:
            print(f"no save folders under {root}")
        return 0

    if args.cmd in ("parse", "validate"):
        sf = SaveFolder(args.folder)
        model = sf.parse(redact=args.redact)
        if args.json:
            _write_json(args.json, model)
        if args.cmd == "validate":
            v = model["validation"]
            for sev in ("errors", "warnings", "infos"):
                for item in v[sev]:
                    print(f"{sev[:-1].upper()}: {item['code']}: {item['message']}"
                          + (f" [{item['where']}]" if item.get("where") else ""))
            if not any(v[k] for k in v):
                print("validation: PASS (no issues)")
            print(f"RESULT: {'FAIL' if v['errors'] else 'PASS'}")
        else:
            if not args.quiet:
                print(_summary(model))
        return 0 if not model["validation"]["errors"] else 1

    if args.cmd == "rewrite":
        sf = SaveFolder(args.src)
        out = sf.rewrite_to(args.dst, overwrite=args.overwrite)
        print(_summary(out))
        return 0 if not out["validation"]["errors"] else 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
