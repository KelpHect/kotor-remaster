"""Area scene loader: module RIM (ARE/GIT/IFO/PTH) + LYT/WOK + room MDL/MDX,
all read-only from the install, with a TPC/TGA texture cache.

Resource placement (verified by probing this install, build SHA-256
0290889e...74e6): ARE/GIT/IFO live in ``modules/<stem>.rim``, PTH in
``<stem>_s.rim``; LYT/VIS/WOK/MDL/MDX live in the base BIFs addressed by
``chitin.key`` under the area/room resrefs; base textures are TPCs in
``TexturePacks/swpc_tex_tpa.erf`` (and gui pack), lightmaps are TGAs in
``lightmaps*.bif``.
"""

from __future__ import annotations

from . import kotor as K
from . import gffx, lyt, mdl, tpc, wok

def _res(game: K.Game, resref: str, rtype: int, log: list):
    hit = game.find(resref, rtype)
    if hit is None:
        return None
    data, src = hit
    log.append({"resref": resref, "type": rtype, "source": src, "size": len(data)})
    return data


class TextureCache:
    def __init__(self, game: K.Game):
        self.game = game
        self.cache: dict[str, tuple | None] = {}
        self.log: list[dict] = []

    def get(self, resref: str):
        """Return (Image, meta, source) or None; caches misses too."""
        key = resref.lower()
        if key in self.cache:
            return self.cache[key]
        hit = self.game.find_texture(resref) if resref else None
        entry = None
        if hit:
            data, rtype, src = hit
            img, meta = tpc.decode_texture(data, rtype)
            meta = dict(meta)
            meta["resref"] = resref
            meta["source"] = src
            meta["res_type"] = rtype
            entry = (img, meta, src)
            self.log.append(meta)
        self.cache[key] = entry
        return entry


def load_area(game: K.Game, stem: str) -> dict:
    """Load one area from its module RIM stem (e.g. 'end_m01aa')."""
    log: list[dict] = []
    game.use_module(stem)
    game.add_packs()

    ifo_data = _res(game, "module", K.T_IFO, log)
    if ifo_data is None:
        raise ValueError(f"{stem}: no module IFO in RIM")
    ifo = gffx.gff_from_bytes(ifo_data, stem + ".ifo")
    ifo_dict = ifo.as_dict(0)
    area = None
    area_rows = gffx.list_field(ifo, 0, "Mod_Area_list")
    if area_rows and isinstance(area_rows[0].get("Area_Name"), str):
        area = area_rows[0]["Area_Name"]
    if area is None:
        raise ValueError(f"{stem}: Mod_Area_list has no Area_Name")

    are_data = _res(game, area, K.T_ARE, log)
    git_data = _res(game, area, K.T_GIT, log)
    pth_data = _res(game, area, K.T_PTH, log)
    lyt_data = _res(game, area, K.T_LYT, log)
    if are_data is None or git_data is None or lyt_data is None:
        raise ValueError(f"{area}: missing ARE/GIT/LYT")
    are = gffx.gff_from_bytes(are_data, area + ".are")
    git = gffx.gff_from_bytes(git_data, area + ".git")
    pth = gffx.gff_from_bytes(pth_data, area + ".pth") if pth_data else None
    rooms_lyt, hooks = lyt.parse_lyt(lyt_data)

    ifo_entry = (ifo_dict.get("Mod_Entry_X", 0.0), ifo_dict.get("Mod_Entry_Y", 0.0),
                 ifo_dict.get("Mod_Entry_Z", 0.0))
    ifo_dir = (ifo_dict.get("Mod_Entry_Dir_X", 0.0), ifo_dict.get("Mod_Entry_Dir_Y", 1.0))

    tex = TextureCache(game)
    rooms = []
    for r in rooms_lyt:
        key = r["room"].lower()
        origin = (r["x"], r["y"], r["z"])
        wok_data = _res(game, key, K.T_WOK, log)
        mdl_data = _res(game, key, K.T_MDL, log)
        mdx_data = _res(game, key, K.T_MDX, log)
        room = {"name": r["room"], "key": key, "origin": origin,
                "wok": None, "mdl": None, "tris": [], "tex0": None}
        if wok_data:
            room["wok"] = wok.Wok(wok_data, key + ".wok")
        if mdl_data and mdx_data:
            m = mdl.Mdl(mdl_data, mdx_data, key + ".mdl")
            room["mdl"] = m
            room["tris"] = m.world_triangles(origin)
            for _, _, t0, _t1 in room["tris"]:
                if t0:
                    room["tex0"] = t0
                    break
        if room["tex0"]:
            tex.get(room["tex0"])
        rooms.append(room)

    git_lists = {
        "creatures": gffx.list_field(git, 0, "Creature List"),
        "doors": gffx.list_field(git, 0, "Door List"),
        "waypoints": gffx.list_field(git, 0, "WaypointList"),
        "placeables": gffx.list_field(git, 0, "Placeable List"),
    }
    checks = [
        {"name": "gff_check_are", "ok": gffx.gff.check(are) == []},
        {"name": "gff_check_git", "ok": gffx.gff.check(git) == []},
        {"name": "gff_check_ifo", "ok": gffx.gff.check(ifo) == []},
        {"name": "gff_check_pth",
         "ok": (gffx.gff.check(pth) == []) if pth else None},
        {"name": "lyt_rooms_loaded",
         "expected": len(rooms_lyt), "actual": len(rooms),
         "ok": len(rooms_lyt) == len(rooms)},
    ]
    scene = {
        "module": stem,
        "area": area,
        "ifo_entry": ifo_entry,
        "ifo_dir": ifo_dir,
        "rooms": rooms,
        "hooks": hooks,
        "git": git_lists,
        "textures": tex,
        "resource_log": log,
        "checks": checks,
    }
    return scene


def scene_stats(scene: dict) -> dict:
    rooms = scene["rooms"]
    wok_faces = sum(len(r["wok"].faces) for r in rooms if r["wok"])
    wok_verts = sum(len(r["wok"].verts) for r in rooms if r["wok"])
    floor = sum(len(r["wok"].floor_faces()) for r in rooms if r["wok"])
    mdl_tris = sum(len(r["tris"]) for r in rooms)
    mdl_meshes = sum(1 for r in rooms if r["mdl"]
                     for nd in r["mdl"].nodes.values() if nd["type"] == mdl.TRIMESH)
    git = scene["git"]
    return {
        "area": scene["area"],
        "module": scene["module"],
        "rooms": len(rooms),
        "rooms_with_wok": sum(1 for r in rooms if r["wok"]),
        "hooks": len(scene["hooks"]),
        "wok_vertices": wok_verts,
        "wok_triangles": wok_faces,
        "wok_floor_triangles": floor,
        "mdl_meshes": mdl_meshes,
        "mdl_triangles": mdl_tris,
        "git": {k: len(v) for k, v in git.items()},
        "textures_loaded": sum(1 for v in scene["textures"].cache.values() if v),
        "textures_requested": len(scene["textures"].cache),
    }
