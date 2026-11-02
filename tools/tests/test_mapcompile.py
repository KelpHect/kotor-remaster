#!/usr/bin/env python3
"""Synthetic tests for the lane s1-03 map compiler (tools/mapcompile).

All fixtures are synthetic: mini-areas are built in memory (validator
tests) or in a synthetic game tree written with the lane-05 fixture
writers (loader test).  No game data is read or copied here; the read-only
full-corpus run is `python tools/mapcompile/compile.py` (see
docs/recon/19_MAP_COMPILE.md).

Run:  python tools/tests/test_mapcompile.py
  or  python tools/tests/run_all.py
"""

from __future__ import annotations

import os
import shutil
import struct
import sys
import tempfile
import unittest

TESTS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(TESTS, "..", ".."))
for _p in (os.path.join(REPO, "tools"), os.path.join(REPO, "tools", "resources"),
           os.path.join(REPO, "tools", "formats")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from mapcompile import constants as C  # noqa: E402
from mapcompile.parsers import Wok, Mdl, gff_from_bytes  # noqa: E402
from mapcompile.loader import ModuleData, GameStore, load_module  # noqa: E402
from mapcompile.validator import validate_module  # noqa: E402
import kotor_archive as ka  # noqa: E402
from kotor_gff_write import GffStruct, gff_bytes  # noqa: E402

WALKABLE = frozenset({1, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14, 18, 30})
LM_TEX = frozenset({"t01a_lm0"})


# ---------------------------------------------------------------------------
# synthetic binary builders
# ---------------------------------------------------------------------------

def wok_bytes(verts, faces, surfaces, wtype=1, normals=None):
    """Build BWM V1.0 bytes.  verts=[(x,y,z)], faces=[(a,b,c)], surfaces=[id].

    normals: optional explicit per-face normals (the winding fixture flips a
    face while keeping the stored +Z normal, which is what rule R1 checks).
    """
    vc, fc = len(verts), len(faces)
    vo = 136
    fo = vo + vc * 12
    mo = fo + fc * 12
    no = mo + fc * 4
    hdr = bytearray(136)
    hdr[0:8] = b"BWM V1.0"
    struct.pack_into("<I", hdr, 8, wtype)
    vals = [vc, vo, fc, fo, mo, no, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    struct.pack_into("<16I", hdr, 72, *vals)
    out = bytearray(hdr)
    for v in verts:
        out += struct.pack("<3f", *v)
    for f in faces:
        out += struct.pack("<6H", f[0], 0, f[1], 0, f[2], 0)
    for s in surfaces:
        out += struct.pack("<I", s)
    if normals is None:
        normals = []
        for f in faces:
            a, b, c = (verts[i] for i in f)
            u = tuple(b[i] - a[i] for i in range(3))
            v = tuple(c[i] - a[i] for i in range(3))
            cr = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                  u[0] * v[1] - u[1] * v[0])
            normals.append(math_sqrt(cr))
    for n in normals:
        out += struct.pack("<3f", *n)
    return bytes(out)


def math_sqrt(cr):
    L = (cr[0] ** 2 + cr[1] ** 2 + cr[2] ** 2) ** 0.5
    if L == 0:
        return (0.0, 0.0, 0.0)
    return (cr[0] / L, cr[1] / L, cr[2] / L)


def mdx_bytes(uv1_list, stride=40):
    """Build an MDX block of vertex records (pos, normal, uv0, uv1)."""
    out = bytearray()
    for i, (u, v) in enumerate(uv1_list):
        out += struct.pack("<3f", float(i), 0.0, 0.0)      # pos
        out += struct.pack("<3f", 0.0, 0.0, 1.0)            # normal
        out += struct.pack("<2f", 0.25, 0.25)               # uv0
        out += struct.pack("<2f", u, v)                     # uv1
        if stride > 40:
            out += b"\x00" * (stride - 40)
    return bytes(out)


def mdl_bytes(mesh):
    """Build a minimal MDL with one root node (plain) and one mesh child.

    mesh: dict(name, stride, vc, tex, mdxoff, tex0, tex1, bmin, bmax)
    """
    name = mesh["name"]
    node_hdr = 80
    mesh_hdr = 328                      # up to +324 mdx offset
    root_off = 168                      # rel 12 -> file 180
    mesh_off = root_off + node_hdr      # rel 12
    child_arr_off = mesh_off + node_hdr + mesh_hdr   # rel 12
    name_tbl_off = child_arr_off + 4    # rel 12
    str_off = name_tbl_off + 4          # rel 12
    file_len = 12 + str_off + len(name) + 1
    buf = bytearray(file_len)
    struct.pack_into("<3I", buf, 0, 0, file_len - 12, 0)      # file header
    struct.pack_into("<2I", buf, 52, root_off, 2)             # root, node count
    struct.pack_into("<3I", buf, 184, 0, 0, name_tbl_off)     # mdx off/size, name tbl
    struct.pack_into("<2I", buf, 200, 1, 1)                   # name count x2
    # root node (plain, type 0)
    ra = 12 + root_off
    struct.pack_into("<4H", buf, ra, 0, 0, 0, 0)
    struct.pack_into("<I", buf, ra + 12, 0xFFFFFFFF)
    struct.pack_into("<3f", buf, ra + 16, 0.0, 0.0, 0.0)
    struct.pack_into("<4f", buf, ra + 28, 1.0, 0.0, 0.0, 0.0)
    struct.pack_into("<3I", buf, ra + 44, child_arr_off, 1, 1)
    struct.pack_into("<I", buf, 12 + child_arr_off, mesh_off)
    # mesh node
    ma = 12 + mesh_off
    struct.pack_into("<4H", buf, ma, 0x21, 0, 1, 0)
    struct.pack_into("<I", buf, ma + 12, root_off)
    struct.pack_into("<3f", buf, ma + 16, 0.0, 0.0, 0.0)
    struct.pack_into("<4f", buf, ma + 28, 1.0, 0.0, 0.0, 0.0)
    struct.pack_into("<3I", buf, ma + 44, 0, 0, 0)
    mh = ma + 80
    struct.pack_into("<2I", buf, mh + 8, 0, 0)                # face off/count
    struct.pack_into("<3f", buf, mh + 20, *mesh["bmin"])
    struct.pack_into("<3f", buf, mh + 32, *mesh["bmax"])
    buf[mh + 88:mh + 120] = mesh["tex0"].encode("latin-1").ljust(32, b"\x00")
    buf[mh + 120:mh + 152] = mesh["tex1"].encode("latin-1").ljust(32, b"\x00")
    struct.pack_into("<I", buf, mh + 252, mesh["stride"])
    struct.pack_into("<I", buf, mh + 304, mesh["vc"] | (mesh["tex"] << 16))
    struct.pack_into("<I", buf, mh + 324, mesh["mdxoff"])
    # name table + string
    struct.pack_into("<I", buf, 12 + name_tbl_off, str_off)
    buf[12 + str_off:12 + str_off + len(name)] = name.encode("latin-1")
    return bytes(buf)


# ---------------------------------------------------------------------------
# synthetic GFF builders
# ---------------------------------------------------------------------------

def gff_of(file_type, root):
    return gff_from_bytes(gff_bytes(file_type, root), f"synthetic.{file_type}")


def make_ifo(area, x, y, z):
    root = GffStruct(0)
    root.add("Mod_Entry_Area", "resref", area)
    root.add("Mod_Entry_X", "float", x)
    root.add("Mod_Entry_Y", "float", y)
    root.add("Mod_Entry_Z", "float", z)
    root.add("Mod_Entry_Dir_X", "float", 0.0)
    root.add("Mod_Entry_Dir_Y", "float", 1.0)
    alist = GffStruct(0)
    alist.add("Area_Name", "resref", area)
    root.add("Mod_Area_list", "list", [alist])
    return gff_of("IFO ", root)


def make_are(rooms):
    root = GffStruct(0)
    rl = [GffStruct(0) for _ in rooms]
    for r, s in zip(rooms, rl):
        s.add("RoomName", "resref", r)
    root.add("Rooms", "list", rl)
    return gff_of("ARE ", root)


def make_git(doors=(), waypoints=(), cameras=(), transition=True):
    root = GffStruct(0)
    root.add("UseTemplates", "uint8", 1)
    dl = []
    for tag, x, y, z in doors:
        s = GffStruct(0)
        s.add("Tag", "string", tag)
        s.add("X", "float", x)
        s.add("Y", "float", y)
        s.add("Z", "float", z)
        s.add("Bearing", "float", 0.0)
        s.add("LinkedTo", "string", "dest" if transition else "")
        s.add("LinkedToModule", "string", "tmod" if transition else "")
        s.add("LinkedToFlags", "uint8", 1 if transition else 0)
        dl.append(s)
    root.add("Door List", "list", dl)
    root.add("Encounter List", "list", [])
    root.add("TriggerList", "list", [])
    wl = []
    for tag, x, y, z in waypoints:
        s = GffStruct(0)
        s.add("Tag", "string", tag)
        s.add("XPosition", "float", x)
        s.add("YPosition", "float", y)
        s.add("ZPosition", "float", z)
        wl.append(s)
    root.add("WaypointList", "list", wl)
    cl = []
    for x, y, z in cameras:
        s = GffStruct(0)
        s.add("Position", "vector3", (x, y, z))
        cl.append(s)
    root.add("CameraList", "list", cl)
    return gff_of("GIT ", root)


def make_pth(points, edges):
    """points=[(x,y)], edges=[(u,v)] -> PTH GFF (reciprocal pairs expected)."""
    root = GffStruct(0)
    cons = []
    pl = []
    first = 0
    for u, (x, y) in enumerate(points):
        outs = [v for a, v in edges if a == u]
        s = GffStruct(0)
        s.add("X", "float", x)
        s.add("Y", "float", y)
        s.add("First_Conection", "int32", first)
        s.add("Conections", "int32", len(outs))
        for v in outs:
            c = GffStruct(0)
            c.add("Destination", "int32", v)
            cons.append(c)
        first += len(outs)
        pl.append(s)
    root.add("Path_Points", "list", pl)
    root.add("Path_Conections", "list", cons)
    return gff_of("PTH ", root)


# ---------------------------------------------------------------------------
# synthetic mini-area
# ---------------------------------------------------------------------------

ROOM = "t01a"


def good_module(**over):
    verts = [(0, 0, 0), (4, 0, 0), (4, 4, 0), (0, 4, 0)]
    faces = [(0, 1, 2), (0, 2, 3)]
    surfaces = [10, 10]
    kw = dict(
        verts=verts, faces=faces, surfaces=surfaces,
        lyt_rooms=[{"room": "T01a", "x": 2.0, "y": 2.0, "z": 0.0}],
        lyt_hooks=[{"room": "T01a", "node": "Door_01", "x": 2.0, "y": 0.0,
                    "z": 0.0, "quat": (1.0, 0.0, 0.0, 0.0)}],
        uv1=[(0.1, 0.1), (0.9, 0.1), (0.9, 0.9), (0.1, 0.9)],
        ifo=(2.0, 2.0, 0.0),
        doors=[("door1", 2.0, 0.0, 0.0)],
        waypoints=[("wp1", 1.0, 1.0, 0.0)],
        cameras=[(2.0, 2.0, 1.0)],
        pth_points=[(1.0, 1.0), (3.0, 3.0)],
        pth_edges=[(0, 1), (1, 0)],
    )
    kw.update(over)
    wok = Wok(wok_bytes(kw["verts"], kw["faces"], kw["surfaces"],
                        normals=kw.get("normals")), "t01a.wok")
    mdx = mdx_bytes(kw["uv1"])
    mesh = dict(name="mesh0", stride=40, vc=len(kw["uv1"]), tex=2, mdxoff=0,
                tex0="tex0", tex1="t01a_lm0", bmin=(-1, -1, -1), bmax=(5, 5, 1))
    mdl = Mdl(mdl_bytes(mesh), "t01a.mdl")
    return ModuleData(
        module="tmod", area="t01aa",
        ifo=make_ifo("t01aa", *kw["ifo"]),
        are=make_are([ROOM]),
        git=make_git(doors=kw["doors"], waypoints=kw["waypoints"],
                     cameras=kw["cameras"],
                     transition=kw.get("transition", True)),
        pth=make_pth(kw["pth_points"], kw["pth_edges"]),
        lyt_rooms=kw["lyt_rooms"], lyt_hooks=kw["lyt_hooks"], vis={ROOM: []},
        woks={ROOM: wok}, mdls={ROOM: mdl}, mdxs={ROOM: mdx},
        are_rooms=[ROOM], walkable_surfaces=WALKABLE, lightmap_textures=LM_TEX,
    )


def hard_codes(report):
    return {e["code"] for e in report["entries"] if e["severity"] == "hard"}


class ValidatorFixtureTests(unittest.TestCase):
    def test_good_mini_area_is_clean(self):
        rep = validate_module(good_module())
        self.assertEqual(rep["status"], "clean", rep["entries"])
        self.assertEqual(rep["hard_failures"], 0)
        self.assertEqual(rep["stats"]["pth_components"], 1)
        self.assertEqual(rep["stats"]["walkmesh_components"][ROOM], 1)
        self.assertGreater(rep["stats"]["uv1_checked"], 0)

    def test_rule1_inverted_winding_hard_fails(self):
        # first face index order flipped while the stored normal stays +Z:
        # cross(v1-v0, v2-v0) . normal < 0 -> inverted vs the RH Z-up convention
        md = good_module(faces=[(0, 2, 1), (0, 2, 3)],
                         normals=[(0.0, 0.0, 1.0), (0.0, 0.0, 1.0)])
        rep = validate_module(md)
        self.assertIn(C.R_WINDING, hard_codes(rep))
        self.assertEqual(rep["status"], "hard_failed")

    def test_rule2_entry_outside_bounds_hard_fails(self):
        md = good_module(ifo=(100.0, 100.0, 0.0))
        rep = validate_module(md)
        self.assertIn(C.R_BOUNDS, hard_codes(rep))

    def test_rule2_are_room_missing_from_lyt_hard_fails(self):
        md = good_module()
        md.are_rooms = ["t01a", "ghost"]
        rep = validate_module(md)
        self.assertIn(C.R_BOUNDS, hard_codes(rep))

    def test_rule3_disconnected_walkmesh_hard_fails(self):
        # two disjoint quads: 8 verts, 4 faces, all walkable, no shared edge
        verts = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0),
                 (10, 10, 0), (11, 10, 0), (11, 11, 0), (10, 11, 0)]
        faces = [(0, 1, 2), (0, 2, 3), (4, 5, 6), (4, 6, 7)]
        md = good_module(verts=verts, faces=faces, surfaces=[10] * 4,
                         lyt_rooms=[{"room": "T01a", "x": 0.5, "y": 0.5, "z": 0.0}])
        rep = validate_module(md)
        self.assertIn(C.R_WALKMESH, hard_codes(rep))
        self.assertEqual(rep["stats"]["walkmesh_components"][ROOM], 2)

    def test_rule4_bad_destination_hard_fails(self):
        md = good_module(pth_edges=[(0, 99), (1, 0)])
        rep = validate_module(md)
        self.assertIn(C.R_PTH, hard_codes(rep))

    def test_rule4_disconnected_pth_hard_fails(self):
        md = good_module(pth_edges=[])
        rep = validate_module(md)
        self.assertIn(C.R_PTH, hard_codes(rep))
        self.assertEqual(rep["stats"]["pth_components"], 2)

    def test_rule5_lightmap_uv_outside_unit_hard_fails(self):
        md = good_module(uv1=[(0.1, 0.1), (1.5, 0.1), (0.9, 0.9), (0.1, 0.9)])
        rep = validate_module(md)
        self.assertIn(C.R_LIGHTMAP_UV, hard_codes(rep))

    def test_rule6_anchor_outside_bounds_hard_fails(self):
        md = good_module(doors=[("door1", 100.0, 100.0, 0.0)],
                         lyt_hooks=[{"room": "T01a", "node": "Door_01",
                                     "x": 100.0, "y": 100.0, "z": 0.0,
                                     "quat": (1.0, 0.0, 0.0, 0.0)}])
        rep = validate_module(md)
        self.assertIn(C.R_ANCHOR, hard_codes(rep))

    def test_rule6_door_hook_offset_triage_not_hard(self):
        # 51 mm-class offset: known shipped outlier class (recon/11 end_door06)
        md = good_module(doors=[("door1", 2.051, 0.0, 0.0)])
        rep = validate_module(md)
        self.assertNotIn(C.R_ANCHOR, hard_codes(rep))
        codes = {e["code"] for e in rep["entries"]}
        self.assertIn(C.T_DOOR_HOOK_OFFSET, codes)

    def test_uv_sentinel_triaged(self):
        ub = struct.unpack("<f", struct.pack("<I", C.UV_SENTINEL_BITS))[0]
        md = good_module(uv1=[(ub, 0.1), (0.9, 0.1), (0.9, 0.9), (0.1, 0.9)])
        rep = validate_module(md)
        self.assertNotIn(C.R_LIGHTMAP_UV, hard_codes(rep))
        self.assertEqual(rep["stats"]["uv1_sentinels"], 1)

    def test_retail_profile_triages_multi_island_walkmesh(self):
        # The strict profile hard-fails the same two disjoint quads above;
        # the retail profile records the shipped multi-level split as triage.
        verts = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0),
                 (10, 10, 0), (11, 10, 0), (11, 11, 0), (10, 11, 0)]
        faces = [(0, 1, 2), (0, 2, 3), (4, 5, 6), (4, 6, 7)]
        md = good_module(verts=verts, faces=faces, surfaces=[10] * 4,
                         ifo=(0.5, 0.5, 0.0),
                         lyt_rooms=[{"room": "T01a", "x": 0.5, "y": 0.5, "z": 0.0}])
        rep = validate_module(md, profile="retail")
        self.assertEqual(rep["status"], "triaged")
        self.assertNotIn(C.R_WALKMESH, hard_codes(rep))
        self.assertIn(C.T_WALKMESH_MULTI,
                      {e["code"] for e in rep["entries"]})

    def test_retail_profile_triages_pth_split(self):
        md = good_module(pth_edges=[])
        rep = validate_module(md, profile="retail")
        self.assertEqual(rep["status"], "triaged")
        self.assertNotIn(C.R_PTH, hard_codes(rep))
        self.assertIn(C.T_PTH_MULTI_COMPONENT,
                      {e["code"] for e in rep["entries"]})

    def test_retail_profile_triages_lightmap_wrap(self):
        md = good_module(uv1=[(0.1, 0.1), (1.5, 0.1), (0.9, 0.9), (0.1, 0.9)])
        rep = validate_module(md, profile="retail")
        self.assertEqual(rep["status"], "triaged")
        self.assertNotIn(C.R_LIGHTMAP_UV, hard_codes(rep))
        self.assertIn(C.T_LIGHTMAP_UV_WRAP,
                      {e["code"] for e in rep["entries"]})

    def test_retail_profile_static_door_needs_no_hook(self):
        # LinkedTo/Module empty and flags 0: cosmetic door, no transition.
        md = good_module(lyt_hooks=[], transition=False)
        rep = validate_module(md, profile="retail")
        self.assertEqual(rep["status"], "triaged")
        self.assertNotIn(C.R_ANCHOR, hard_codes(rep))
        self.assertIn(C.T_DOOR_NO_HOOK,
                      {e["code"] for e in rep["entries"]})


# ---------------------------------------------------------------------------
# loader test over a synthetic game tree (lane-05 fixture writers)
# ---------------------------------------------------------------------------

def two_da_v2b(columns, rows):
    """Minimal V2.b 2DA: columns=[name], rows=[[cell,...]] (all strings)."""
    col_blob = b"".join(c.encode() + b"\t" for c in columns) + b"\x00"
    out = bytearray(b"2DA V2.b\r\n")
    out += col_blob
    out += struct.pack("<I", len(rows))
    for r in rows:
        out += r[0].encode() + b"\t"
    strings = bytearray()
    offsets = []
    for r in rows:
        for cell in r[1:]:
            offsets.append(len(strings))
            strings += cell.encode() + b"\x00"
    for o in offsets:
        out += struct.pack("<H", o)
    out += struct.pack("<H", len(strings))
    out += strings
    return bytes(out)


def build_synthetic_game(root):
    """Write chitin.key + data/*.bif + modules/tmod{,_s}.rim under root."""
    os.makedirs(os.path.join(root, "data"), exist_ok=True)
    os.makedirs(os.path.join(root, "modules"), exist_ok=True)
    area = "t01aa"
    md = good_module()  # reuse builders for payloads
    wok = wok_bytes([(0, 0, 0), (4, 0, 0), (4, 4, 0), (0, 4, 0)],
                    [(0, 1, 2), (0, 2, 3)], [10, 10])
    mdx = mdx_bytes([(0.1, 0.1), (0.9, 0.1), (0.9, 0.9), (0.1, 0.9)])
    mesh = dict(name="mesh0", stride=40, vc=4, tex=2, mdxoff=0,
                tex0="tex0", tex1="t01a_lm0", bmin=(-1, -1, -1), bmax=(5, 5, 1))
    mdl = mdl_bytes(mesh)
    lyt = ("#MAXLAYOUT ASCII\r\nfiledependancy T.max\r\nbeginlayout\r\n"
           "   roomcount 1\r\n      T01a 2.0 2.0 0.0\r\n"
           "   doorhookcount 1\r\n      T01a Door_01 0 2.0 0.0 0.0 1.0 0.0 0.0 0.0\r\n"
           "donelayout\r\n").encode("latin-1")
    vis = b"t01a 0\r\n"
    txi = b"islightmap 1\r\n"
    surfacemat = two_da_v2b(["walk"],
                            [[str(i), "1" if i in WALKABLE else "0"]
                             for i in range(31)])
    bifs = {
        "data\\layouts.bif": [(0, C.T_LYT, lyt)],
        "data\\lightmaps.bif": [(0, C.T_VIS, vis), (1, 2022, txi)],
        "data\\models.bif": [(0, C.T_BWM_WOK, wok), (1, C.T_MDL, mdl),
                              (2, C.T_MDX, mdx)],
        "data\\2da.bif": [(0, C.T_2DA, surfacemat)],
    }
    bif_files = []
    keys = []
    for bi, (name, resources) in enumerate(bifs.items()):
        path = os.path.join(root, name.replace("\\", "/"))
        ka.write_bif(path, resources)
        bif_files.append({"name": name, "size": os.path.getsize(path), "drives": 1})
        for ti, (_rid, rtype, _data) in enumerate(resources):
            resref = {C.T_LYT: area, C.T_VIS: area, 2022: "t01a_lm0",
                      C.T_BWM_WOK: ROOM, C.T_MDL: ROOM, C.T_MDX: ROOM,
                      C.T_2DA: "surfacemat"}[rtype]
            keys.append((resref, rtype, bi, ti))
    ka.write_key(os.path.join(root, "chitin.key"), bif_files, keys)
    ka.write_rim(os.path.join(root, "modules", "tmod.rim"), [
        (area, C.T_ARE, gff_bytes("ARE ", _are_root())),
        (area, C.T_GIT, gff_bytes("GIT ", _git_root())),
        ("module", C.T_IFO, gff_bytes("IFO ", _ifo_root())),
    ])
    ka.write_rim(os.path.join(root, "modules", "tmod_s.rim"), [
        (area, C.T_PTH, gff_bytes("PTH ", _pth_root())),
    ])
    # swkotor.exe sentinel is only checked by compile.py, not by GameStore
    return root


def _ifo_root():
    root = GffStruct(0)
    root.add("Mod_Entry_Area", "resref", "t01aa")
    root.add("Mod_Entry_X", "float", 2.0)
    root.add("Mod_Entry_Y", "float", 2.0)
    root.add("Mod_Entry_Z", "float", 0.0)
    root.add("Mod_Entry_Dir_X", "float", 0.0)
    root.add("Mod_Entry_Dir_Y", "float", 1.0)
    alist = GffStruct(0)
    alist.add("Area_Name", "resref", "t01aa")
    root.add("Mod_Area_list", "list", [alist])
    return root


def _are_root():
    root = GffStruct(0)
    s = GffStruct(0)
    s.add("RoomName", "resref", ROOM)
    root.add("Rooms", "list", [s])
    return root


def _git_root():
    root = GffStruct(0)
    root.add("UseTemplates", "uint8", 1)
    d = GffStruct(0)
    d.add("Tag", "string", "door1")
    d.add("X", "float", 2.0)
    d.add("Y", "float", 0.0)
    d.add("Z", "float", 0.0)
    d.add("Bearing", "float", 0.0)
    root.add("Door List", "list", [d])
    w = GffStruct(0)
    w.add("Tag", "string", "wp1")
    w.add("XPosition", "float", 1.0)
    w.add("YPosition", "float", 1.0)
    w.add("ZPosition", "float", 0.0)
    root.add("WaypointList", "list", [w])
    root.add("Encounter List", "list", [])
    root.add("TriggerList", "list", [])
    c = GffStruct(0)
    c.add("Position", "vector3", (2.0, 2.0, 1.0))
    root.add("CameraList", "list", [c])
    return root


def _pth_root():
    root = GffStruct(0)
    p0 = GffStruct(0)
    p0.add("X", "float", 1.0)
    p0.add("Y", "float", 1.0)
    p0.add("First_Conection", "int32", 0)
    p0.add("Conections", "int32", 1)
    p1 = GffStruct(0)
    p1.add("X", "float", 3.0)
    p1.add("Y", "float", 3.0)
    p1.add("First_Conection", "int32", 1)
    p1.add("Conections", "int32", 1)
    root.add("Path_Points", "list", [p0, p1])
    c0 = GffStruct(0)
    c0.add("Destination", "int32", 1)
    c1 = GffStruct(0)
    c1.add("Destination", "int32", 0)
    root.add("Path_Conections", "list", [c0, c1])
    return root


class LoaderSyntheticGameTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="mapcompile_test_")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_load_and_validate_synthetic_module_clean(self):
        build_synthetic_game(self.tmp)
        store = GameStore(self.tmp)
        self.assertEqual(store.modules(), ["tmod"])
        self.assertIn(10, store.walkable_surfaces)
        self.assertIn("t01a_lm0", store.lightmap_textures)
        md = load_module(store, "tmod")
        self.assertEqual(md.area, "t01aa")
        self.assertEqual(sorted(md.woks), [ROOM])
        rep = validate_module(md)
        self.assertEqual(rep["status"], "clean", rep["entries"])
        self.assertEqual(rep["stats"]["walkmesh_components"][ROOM], 1)
        self.assertEqual(rep["stats"]["pth_components"], 1)
        self.assertEqual(rep["stats"]["ifo_entry_floor_dz"], 0.0)

    def test_loader_detects_missing_pth(self):
        build_synthetic_game(self.tmp)
        os.remove(os.path.join(self.tmp, "modules", "tmod_s.rim"))
        ka.write_rim(os.path.join(self.tmp, "modules", "tmod_s.rim"), [])
        store = GameStore(self.tmp)
        md = load_module(store, "tmod")
        self.assertIsNone(md.pth)
        rep = validate_module(md)
        self.assertEqual(rep["status"], "triaged")  # T_PTH_MULTI_COMPONENT, not hard
        self.assertNotIn(C.R_PTH, hard_codes(rep))


if __name__ == "__main__":
    unittest.main(verbosity=2)
