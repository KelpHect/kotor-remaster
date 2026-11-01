"""Loader: read one module pair + base-BIF area resources from the install.

Read-only.  Resource homes (verified against the install, build
0290889e...74437e6, via artifacts/analysis/coordinates2/extract/manifest.json):

  modules/<mod>.rim      ARE (2012), GIT (2023), IFO (2014, resref "module")
  modules/<mod>_s.rim    PTH (3003, resref = area)
  data/layouts.bif       LYT (3000, resref = area, "#MAXLAYOUT" text)
  data/lightmaps.bif     VIS (3001, resref = area)
  data/models.bif        per-room WOK (2016), MDL (2002), MDX (3008)
  data/2da.bif           surfacemat.2da (walkable surface ids)
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
for _p in (os.path.join(_ROOT, "tools", "resources"),
           os.path.join(_ROOT, "tools", "formats")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import kotor_archive as ka  # noqa: E402
from two_da import parse_2da  # noqa: E402

from . import constants as C  # noqa: E402
from .parsers import Mdl, Wok, gff_from_bytes, parse_lyt, parse_vis  # noqa: E402


class LoadError(ValueError):
    pass


@dataclass
class ModuleData:
    module: str
    area: str
    ifo: object
    are: object
    git: object
    pth: object | None
    lyt_rooms: list
    lyt_hooks: list
    vis: dict
    woks: dict = field(default_factory=dict)   # room_lower -> Wok
    mdls: dict = field(default_factory=dict)   # room_lower -> Mdl
    mdxs: dict = field(default_factory=dict)   # room_lower -> bytes
    are_rooms: list = field(default_factory=list)  # RoomName values
    walkable_surfaces: frozenset = frozenset()
    lightmap_textures: frozenset = frozenset()  # TXI payload starts b'islightmap'
    sources: dict = field(default_factory=dict)  # resref -> container note


class GameStore:
    """Read-only accessor for chitin.key/BIFs and modules/*.rim."""

    def __init__(self, game_dir: str | os.PathLike):
        self.game_dir = str(game_dir)
        self.key_path = os.path.join(self.game_dir, "chitin.key")
        self.key = ka.read_key(self.key_path)
        self._bifs: dict[str, ka.BifFile] = {}
        self._by_key: dict[tuple[str, int], list] = {}
        for e in self.key.entries:
            self._by_key.setdefault(e.key, []).append(e)
        self._walkable: frozenset | None = None
        self._lightmaps: frozenset | None = None

    # -- BIF tier -----------------------------------------------------------
    def bif(self, name: str) -> ka.BifFile:
        if name not in self._bifs:
            entry = next((b for b in self.key.bifs
                          if b["name"].replace("\\", "/").lower().endswith(name.lower())),
                         None)
            if entry is None:
                raise LoadError(f"{name} not in chitin.key")
            self._bifs[name] = ka.read_bif(ka.bif_path_for(self.game_dir, entry["name"]))
        return self._bifs[name]

    def _record_for(self, e):
        """BIF record for a KEY entry.  Real BIFs store the full resource id
        (bif_index<<20 | table_index) as the record id; the lane-05 fixture
        writer stores the bare table index.  Accept both."""
        bif = self.bif(self.key.bifs[e.bif_index]["name"])
        resid = (e.bif_index << 20) | e.table_index
        return bif, bif.by_id.get(resid) or bif.by_id.get(e.table_index)

    def bif_get(self, resref: str, res_type: int) -> bytes | None:
        for e in self._by_key.get((resref.lower(), res_type), []):
            bif, rec = self._record_for(e)
            if rec is not None:
                return bif.read_record(rec)
        return None

    def bif_has(self, resref: str, res_type: int) -> bool:
        return any(self._record_for(e)[1] is not None
                   for e in self._by_key.get((resref.lower(), res_type), []))

    # -- RIM tier -----------------------------------------------------------
    def modules_dir(self) -> str:
        return os.path.join(self.game_dir, "modules")

    def modules(self) -> list[str]:
        out = []
        for fn in sorted(os.listdir(self.modules_dir())):
            if fn.endswith(".rim") and not fn.endswith("_s.rim"):
                out.append(fn[:-4])
        return out

    def rim(self, module: str, suffix: str = "") -> ka.Archive:
        path = os.path.join(self.modules_dir(), module + suffix + ".rim")
        if not os.path.exists(path):
            raise LoadError(f"missing RIM {path}")
        return ka.read_rim(path)

    # -- 2DA ----------------------------------------------------------------
    @property
    def walkable_surfaces(self) -> frozenset:
        """Surface ids whose surfacemat.2da 'walk' column is '1'."""
        if self._walkable is None:
            data = self.bif_get("surfacemat", C.T_2DA)
            if data is None:
                raise LoadError("surfacemat.2da not found")
            t = parse_2da(data, "surfacemat")
            walk_col = t.column_index("walk")
            ids = {r for r in range(t.row_count)
                   if t.cell(r, walk_col).as_text() == "1"}
            self._walkable = frozenset(ids)
        return self._walkable

    @property
    def lightmap_textures(self) -> frozenset:
        """Texture resrefs whose TXI payload starts with b'islightmap'."""
        if self._lightmaps is None:
            lm_idx = {b["index"] for b in self.key.bifs
                      if os.path.basename(b["name"].replace("\\", "/")).lower()
                      .startswith("lightmaps")}
            names = set()
            for e in self.key.entries:
                if e.bif_index not in lm_idx or e.res_type != 2022:
                    continue
                bif, rec = self._record_for(e)
                if rec is None:
                    continue
                if bif.read_record(rec)[:10].startswith(b"islightmap"):
                    names.add(e.resref.lower())
            self._lightmaps = frozenset(names)
        return self._lightmaps


def _gff_list(g, label):
    for f in g.fields_of(0):
        if f.type == 15 and f.label == label:
            _n, idx = g.list_of(f)
            return [g.as_dict(si) for si in idx]
    return []


def load_module(store: GameStore, module: str) -> ModuleData:
    main = store.rim(module)
    srim = store.rim(module, "_s")

    are_entry = next((e for e in main.entries if e.res_type == C.T_ARE), None)
    git_entry = next((e for e in main.entries if e.res_type == C.T_GIT), None)
    ifo_entry = main.lookup(C.IFO_RESREF, C.T_IFO)
    if are_entry is None or git_entry is None or ifo_entry is None:
        raise LoadError(f"{module}: main RIM missing ARE/GIT/IFO")
    area = are_entry.resref.lower()
    if git_entry.resref.lower() != area:
        raise LoadError(f"{module}: ARE/GIT resref mismatch "
                        f"{are_entry.resref} vs {git_entry.resref}")

    ifo = gff_from_bytes(main.read_entry(ifo_entry), f"{module}.ifo")
    are = gff_from_bytes(main.read_entry(are_entry), f"{area}.are")
    git = gff_from_bytes(main.read_entry(git_entry), f"{area}.git")

    pth = None
    pth_entry = srim.lookup(area, C.T_PTH)
    if pth_entry is not None:
        pth = gff_from_bytes(srim.read_entry(pth_entry), f"{area}.pth")

    lyt_data = store.bif_get(area, C.T_LYT)
    vis_data = store.bif_get(area, C.T_VIS)
    if lyt_data is None:
        raise LoadError(f"{area}: no LYT in layouts.bif")
    lyt_rooms, lyt_hooks = parse_lyt(lyt_data.decode("latin-1"))
    vis = parse_vis(vis_data.decode("latin-1")) if vis_data else {}

    woks, mdls, mdxs = {}, {}, {}
    sources = {"are/git/ifo": f"modules/{module}.rim",
               "pth": f"modules/{module}_s.rim" if pth else None,
               "lyt": "data/layouts.bif", "vis": "data/lightmaps.bif" if vis_data else None}
    for r in lyt_rooms:
        room = r["room"].lower()
        w = store.bif_get(room, C.T_BWM_WOK)
        if w is not None:
            woks[room] = Wok(w, f"{room}.wok")
            sources[f"{room}.wok"] = "data/models.bif"
        m = store.bif_get(room, C.T_MDL)
        if m is not None:
            mdls[room] = Mdl(m, f"{room}.mdl")
            sources[f"{room}.mdl"] = "data/models.bif"
        x = store.bif_get(room, C.T_MDX)
        if x is not None:
            mdxs[room] = x
            sources[f"{room}.mdx"] = "data/models.bif"

    are_rooms = [d.get("RoomName", "") for d in _gff_list(are, "Rooms")]

    return ModuleData(module=module, area=area, ifo=ifo, are=are, git=git,
                      pth=pth, lyt_rooms=lyt_rooms, lyt_hooks=lyt_hooks, vis=vis,
                      woks=woks, mdls=mdls, mdxs=mdxs, are_rooms=are_rooms,
                      walkable_surfaces=store.walkable_surfaces,
                      lightmap_textures=store.lightmap_textures,
                      sources={k: v for k, v in sources.items() if v})
