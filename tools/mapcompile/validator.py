"""Hard-fail validator for one compiled area (lane s1-03).

Coordinate contract under test (frozen in docs/recon/19_MAP_COMPILE.md,
citing docs/recon/03/04/11/12/15 and docs/adr/0002): source space is
right-handed Z-up, 1 unit = 1 m; WOK vertices are world-space; LYT room
origins are MDL model origins; GIT Bearing is a radian yaw; IFO entry is a
ground point with a unit 2D direction.

Rules (hard entries fail the build and produce a nonzero exit; triage
entries are reported known-outlier classes and do not fail it):

  R1_WINDING       every WOK type-1 face must satisfy
                   cross(v1-v0, v2-v0) . normal < -WINDING_INVERT_EPS => inverted
                   (hard).  Zero-area faces (dot ~ 0) are triaged
                   (T_WINDING_DEGENERATE; 123 shipped corpus-wide, 0 inverted).
  R2_BOUNDS        ARE Rooms must exist in the LYT; area geometry bounds
                   (union of per-room WOK bounds and MDL world trimesh
                   extents) must be finite/non-degenerate; the IFO entry
                   point must sit inside the bounds and on the walkmesh
                   floor (|dz| <= ENTRY_FLOOR_DZ).
  R3_WALKMESH      BFS over shared edges of walkable WOK faces per room
                   (surface id walkable per surfacemat.2da 'walk'); more
                   than one component = fail.  Rooms with no walkable
                   faces are triaged (T_ROOM_NO_WOK; e.g. m01aa_07b).
  R4_PTH           every Path_Conections Destination must reference an
                   existing Path_Points node (hard); with points present the
                   undirected graph must be connected (hard; corpus: 1
                   component wherever points exist).  Points > 0.5 m off
                   the walkmesh are triaged (T_PTH_OFF_WALKMESH).
  R5_LIGHTMAP_UV   for every room MDL mesh whose texture1 is a lightmap
                   (TXI payload starts with b'islightmap'), every uv1 must
                   lie in [0,1] (+-UV_EPS); the 0xFEFFEEEE sentinel is
                   triaged (T_UV_SENTINEL, recon/15).
  R6_ANCHOR        every GIT door/waypoint/trigger/encounter/camera must be
                   inside the area geometry bounds or on the walkmesh
                   (<= ON_WALKMESH_TOL); doors must have a LYT hook within
                   HOOK_TOL_HARD (hard) -- offsets above HOOK_TOL_TRIAGE are
                   triaged (T_DOOR_HOOK_OFFSET; known 51 mm end_door06
                   outlier, recon/11).  Inside-bounds-but-off-walkmesh
                   anchors are triaged (T_ANCHOR_OFF_WALKMESH).
"""

from __future__ import annotations

import math
import struct
from collections import defaultdict

from . import constants as C
from .parsers import dist_to_tri2d, face_cross_dot, node_world, point_in_tri2d


def _entry(rule, code, severity, message, refs=()):
    return {"rule": rule, "code": code, "severity": severity,
            "message": message, "refs": list(refs)}


def _pos_of(d):
    """Extract (x, y, z) from a GIT instance dict across field conventions."""
    if "X" in d and "Y" in d:
        return d["X"], d["Y"], d.get("Z", 0.0)
    if "XPosition" in d and "YPosition" in d:
        return d["XPosition"], d["YPosition"], d.get("ZPosition", 0.0)
    p = d.get("Position")
    if isinstance(p, tuple) and len(p) == 3:
        return p
    return None


def _git_lists(git, labels):
    out = {}
    for f in git.fields_of(0):
        if f.type == 15 and f.label in labels:
            _n, idx = git.list_of(f)
            out[f.label] = [git.as_dict(si) for si in idx]
    return out


def _mesh_tex1(mdl, nd):
    h = 12 + nd["off"] + 80
    return mdl.buf[h + 120:h + 152].split(b"\0")[0].decode("latin-1").lower()


class AreaGeom:
    """Precomputed area-level geometry: bounds + walkable face index."""

    def __init__(self, md):
        self.md = md
        ext = [w.bounds() for w in md.woks.values() if w.bounds()]
        # MDL world extents: trimesh node bboxes through the LYT-origin +
        # node chain (recon/04 §4: world = T(room_origin) . parent->child)
        origins = {r["room"].lower(): r for r in md.lyt_rooms}
        for room, mdl in md.mdls.items():
            r = origins.get(room)
            if r is None:
                continue
            pts = []
            for nd in mdl.nodes.values():
                if nd["type"] != 33:
                    continue
                a = 12 + nd["off"] + 80
                bmin = struct.unpack_from("<3f", mdl.buf, a + 20)
                bmax = struct.unpack_from("<3f", mdl.buf, a + 32)
                R, t = node_world(mdl, nd)
                for i in (0, 1):
                    for j in (0, 1):
                        for k in (0, 1):
                            c = (bmax[0] if i else bmin[0],
                                 bmax[1] if j else bmin[1],
                                 bmax[2] if k else bmin[2])
                            pts.append(tuple(
                                r["x"] + t[q] + sum(R[q][s] * c[s] for s in range(3))
                                for q in range(3)))
            if pts:
                ext.append((min(p[0] for p in pts), min(p[1] for p in pts),
                            min(p[2] for p in pts), max(p[0] for p in pts),
                            max(p[1] for p in pts), max(p[2] for p in pts)))
        self.bounds = None
        if ext:
            self.bounds = (min(x[0] for x in ext), min(x[1] for x in ext),
                           min(x[2] for x in ext), max(x[3] for x in ext),
                           max(x[4] for x in ext), max(x[5] for x in ext))
        # room -> list of (tri2d, verts) for walkable faces
        self.walk_faces = {}
        for room, w in md.woks.items():
            faces = []
            for fi, f in enumerate(w.faces):
                sid = w.surface[fi] if fi < len(w.surface) else 0
                walkable = sid in md.walkable_surfaces
                if not md.walkable_surfaces and w.normals:
                    walkable = w.normals[fi][2] > 0.5
                if not walkable:
                    continue
                vs = [w.verts[i] for i in f]
                faces.append(([(v[0], v[1]) for v in vs], vs))
            self.walk_faces[room] = faces

    def inside_bounds(self, x, y, z=None, tol=C.BOUNDS_TOL):
        if self.bounds is None:
            return False
        b = self.bounds
        ok = (b[0] - tol <= x <= b[3] + tol) and (b[1] - tol <= y <= b[4] + tol)
        if z is not None:
            ok = ok and (b[2] - tol <= z <= b[5] + tol)
        return ok

    def bounds_excess(self, x, y):
        """How far (m) the point lies outside the area bounds in XY (0 if inside)."""
        if self.bounds is None:
            return float("inf")
        b = self.bounds
        return max(0.0, b[0] - x, x - b[3], b[1] - y, y - b[4])

    def walk_dist(self, x, y):
        """0 if (x,y) lies in a walkable face, else distance to the nearest."""
        best = float("inf")
        for faces in self.walk_faces.values():
            for tri, _vs in faces:
                if point_in_tri2d(x, y, tri) is not None:
                    return 0.0
                best = min(best, dist_to_tri2d(x, y, tri))
        return best

    def floor_z(self, x, y):
        """Interpolated walkable-face z at (x,y), or None."""
        for faces in self.walk_faces.values():
            for tri, vs in faces:
                bary = point_in_tri2d(x, y, tri)
                if bary is not None:
                    return sum(bary[k] * vs[k][2] for k in range(3))
        return None


# --------------------------------------------------------------------------
# rules
# --------------------------------------------------------------------------

def rule_winding(md, geom, entries, stats):
    inverted = []
    degen = 0
    min_dot = float("inf")
    faces = 0
    for room, w in md.woks.items():
        if w.type != 1:
            continue
        for fi in range(w.fc):
            faces += 1
            dot, area2 = face_cross_dot(w, fi)
            if dot is None:
                continue
            min_dot = min(min_dot, dot)
            if dot < -C.WINDING_INVERT_EPS:
                inverted.append((room, fi, dot))
            elif area2 < C.DEGENERATE_AREA or dot <= C.WINDING_DOT_MIN:
                degen += 1
    stats["wok_faces"] = faces
    stats["wok_min_dot"] = None if min_dot == float("inf") else min_dot
    stats["wok_degenerate"] = degen
    if inverted:
        entries.append(_entry(C.R_WINDING, C.R_WINDING, "hard",
                              f"{len(inverted)} WOK faces inverted vs RH Z-up "
                              f"(cross.normal < -{C.WINDING_INVERT_EPS:g}); "
                              f"worst dot={min(b[2] for b in inverted):.3g}",
                              [f"{r}.wok face {fi}" for r, fi, _ in inverted[:20]]))
    if degen:
        entries.append(_entry(C.R_WINDING, C.T_WINDING_DEGENERATE, "triage",
                              f"{degen} degenerate (zero-area) WOK faces", []))


def rule_bounds(md, geom, entries, stats):
    lyt_rooms = {r["room"].lower(): r for r in md.lyt_rooms}
    missing = [r for r in md.are_rooms if r and r.lower() not in lyt_rooms]
    if missing:
        entries.append(_entry(C.R_BOUNDS, C.R_BOUNDS, "hard",
                              f"ARE Rooms without a LYT room: {missing}", missing))
    no_wok = [r for r in lyt_rooms if r not in md.woks]
    for r in no_wok:
        entries.append(_entry(C.R_BOUNDS, C.T_ROOM_NO_WOK, "triage",
                              f"LYT room {r} has no WOK in models.bif", [r]))
    if geom.bounds is None:
        entries.append(_entry(C.R_BOUNDS, C.R_BOUNDS, "hard",
                              "no geometry: area bounds undefined", []))
    else:
        b = geom.bounds
        if not all(math.isfinite(v) for v in b) or (b[3] - b[0]) < 1e-6 or (b[4] - b[1]) < 1e-6:
            entries.append(_entry(C.R_BOUNDS, C.R_BOUNDS, "hard",
                                  f"degenerate area bounds {b}", []))
    d = md.ifo.as_dict(0)
    entry = (d.get("Mod_Entry_X"), d.get("Mod_Entry_Y"), d.get("Mod_Entry_Z"))
    stats["ifo_entry"] = list(entry) if None not in entry else None
    if None in entry:
        entries.append(_entry(C.R_BOUNDS, C.R_BOUNDS, "hard",
                              "IFO missing Mod_Entry_X/Y/Z", []))
    else:
        x, y, z = entry
        if not geom.inside_bounds(x, y):
            entries.append(_entry(C.R_BOUNDS, C.R_BOUNDS, "hard",
                                  f"IFO entry ({x:.3f},{y:.3f},{z:.3f}) outside area "
                                  f"bounds {geom.bounds}", ["module.ifo"]))
        else:
            fz = geom.floor_z(x, y)
            dz = None if fz is None else z - fz
            stats["ifo_entry_floor_dz"] = dz
            if fz is None or abs(dz) > C.ENTRY_FLOOR_DZ:
                entries.append(_entry(C.R_BOUNDS, C.R_BOUNDS, "hard",
                                      f"IFO entry not on walkmesh floor "
                                      f"(floor_z={fz}, dz={dz})", ["module.ifo"]))


def rule_walkmesh(md, geom, entries, stats):
    stats["walkmesh_components"] = {}
    stats["walkable_faces"] = {}
    for room, w in md.woks.items():
        if w.type != 1:
            continue
        walkable = [fi for fi in range(w.fc)
                    if (w.surface[fi] if fi < len(w.surface) else 0) in md.walkable_surfaces]
        if not walkable:
            walkable = [fi for fi in range(w.fc)
                        if w.normals and w.normals[fi][2] > 0.5]
        if not walkable:
            entries.append(_entry(C.R_WALKMESH, C.T_ROOM_NO_WOK, "triage",
                                  f"room {room}: WOK has no walkable faces "
                                  f"({w.fc} faces, type {w.type})", [f"{room}.wok"]))
            continue
        edge_faces = defaultdict(list)
        for fi in walkable:
            f = w.faces[fi]
            for a, b in ((f[0], f[1]), (f[1], f[2]), (f[2], f[0])):
                edge_faces[frozenset((a, b))].append(fi)
        adj = defaultdict(set)
        for fl in edge_faces.values():
            for i in range(len(fl)):
                for j in range(i + 1, len(fl)):
                    adj[fl[i]].add(fl[j])
                    adj[fl[j]].add(fl[i])
        seen = set()
        comps = 0
        for fi in walkable:
            if fi in seen:
                continue
            comps += 1
            stack = [fi]
            seen.add(fi)
            while stack:
                u = stack.pop()
                for v in adj[u]:
                    if v not in seen:
                        seen.add(v)
                        stack.append(v)
        stats["walkmesh_components"][room] = comps
        stats["walkable_faces"][room] = len(walkable)
        if comps > 1:
            entries.append(_entry(C.R_WALKMESH, C.R_WALKMESH, "hard",
                                  f"room {room}: walkmesh splits into {comps} disconnected "
                                  f"components over {len(walkable)} walkable faces",
                                  [f"{room}.wok"]))


def _pth_graph(md):
    pth = md.pth
    if pth is None:
        return None
    pts, cons, idxs = [], [], []
    for f in pth.fields_of(0):
        if f.type == 15 and f.label == "Path_Points":
            _n, idx = pth.list_of(f)
            idxs = idx
            pts = [pth.as_dict(si) for si in idx]
        elif f.type == 15 and f.label == "Path_Conections":
            _n, idx = pth.list_of(f)
            cons = [pth.as_dict(si)["Destination"] for si in idx]
    edges = []
    for u, si in enumerate(idxs):
        d = pth.as_dict(si)
        k = d.get("Conections", 0)
        first = d.get("First_Conection", 0)
        for q in range(k):
            edges.append((u, cons[first + q]) if first + q < len(cons) else (u, -1))
    return pts, cons, edges


def rule_pth(md, geom, entries, stats):
    g = _pth_graph(md)
    if g is None:
        entries.append(_entry(C.R_PTH, C.T_PTH_MULTI_COMPONENT, "triage",
                              "no PTH in module _s RIM", []))
        stats["pth"] = None
        return
    pts, cons, edges = g
    stats["pth_points"] = len(pts)
    stats["pth_edges"] = len(edges)
    bad = [(u, v) for u, v in edges if v < 0 or v >= len(pts)]
    if bad:
        entries.append(_entry(C.R_PTH, C.R_PTH, "hard",
                              f"{len(bad)} PTH edges reference missing nodes "
                              f"(of {len(pts)} points)",
                              [f"edge {u}->{v}" for u, v in bad[:20]]))
    if pts:
        adj = defaultdict(set)
        for u, v in edges:
            if 0 <= v < len(pts):
                adj[u].add(v)
                adj[v].add(u)
        seen = set()
        comps = []
        for i in range(len(pts)):
            if i in seen:
                continue
            stack = [i]
            seen.add(i)
            comp = []
            while stack:
                u = stack.pop()
                comp.append(u)
                for v in adj[u]:
                    if v not in seen:
                        seen.add(v)
                        stack.append(v)
            comps.append(comp)
        stats["pth_components"] = len(comps)
        stats["pth_component_sizes"] = sorted((len(c) for c in comps), reverse=True)
        if len(comps) > 1:
            entries.append(_entry(C.R_PTH, C.R_PTH, "hard",
                                  f"PTH graph has {len(comps)} disconnected components "
                                  f"(sizes {stats['pth_component_sizes'][:10]})", []))
        off = []
        for i, p in enumerate(pts):
            x, y = p.get("X"), p.get("Y")
            if x is None or y is None:
                off.append((i, None))
                continue
            d = geom.walk_dist(x, y)
            if d > C.PTH_POINT_WALK_TOL:
                off.append((i, d))
        stats["pth_points_off_walkmesh"] = len(off)
        for i, d in off[:20]:
            entries.append(_entry(C.R_PTH, C.T_PTH_OFF_WALKMESH, "triage",
                                  f"PTH point {i} is {d:.3f} m off the walkmesh",
                                  [f"pth point {i}"]))


def rule_lightmap_uv(md, geom, entries, stats):
    checked = 0
    bad = []
    sentinels = 0
    skipped = 0
    for room, mdl in md.mdls.items():
        mdx = md.mdxs.get(room)
        if mdx is None:
            continue
        for nd in mdl.mesh_nodes():
            m = nd["mesh"]
            if m["tex_count"] < 2 or m["vertex_count"] == 0 or m["stride"] < 40:
                continue
            tex1 = _mesh_tex1(mdl, nd)
            if tex1 not in md.lightmap_textures:
                skipped += 1
                continue
            base, stride = m["mdx_offset"], m["stride"]
            for i in range(m["vertex_count"]):
                off = base + i * stride + 32
                if off + 8 > len(mdx):
                    entries.append(_entry(C.R_LIGHTMAP_UV, C.R_LIGHTMAP_UV, "hard",
                                          f"{room}.mdl node {nd['name']!r}: uv1 record {i} "
                                          f"outside MDX", [f"{room}.mdl/{nd['name']}#{i}"]))
                    break
                ub, vb = struct.unpack_from("<2I", mdx, off)
                u, v = struct.unpack_from("<2f", mdx, off)
                checked += 1
                if ub == C.UV_SENTINEL_BITS or vb == C.UV_SENTINEL_BITS:
                    sentinels += 1
                    continue
                if not (-C.UV_EPS <= u <= 1 + C.UV_EPS and -C.UV_EPS <= v <= 1 + C.UV_EPS):
                    bad.append((room, nd["name"], i, u, v))
    stats["uv1_checked"] = checked
    stats["uv1_sentinels"] = sentinels
    stats["uv1_meshes_skipped_nonlightmap"] = skipped
    stats["uv1_out_of_range"] = len(bad)
    for room, name, vi, u, v in bad[:20]:
        entries.append(_entry(C.R_LIGHTMAP_UV, C.R_LIGHTMAP_UV, "hard",
                              f"lightmap uv1 outside [0,1]: {room}.mdl node {name!r} "
                              f"vertex {vi} uv=({u:.6g},{v:.6g})",
                              [f"{room}.mdl/{name}#{vi}"]))
    if len(bad) > 20:
        entries.append(_entry(C.R_LIGHTMAP_UV, C.R_LIGHTMAP_UV, "hard",
                              f"... and {len(bad) - 20} more uv1 violations", []))
    if sentinels:
        entries.append(_entry(C.R_LIGHTMAP_UV, C.T_UV_SENTINEL, "triage",
                              f"{sentinels} uv1 sentinel values 0x{C.UV_SENTINEL_BITS:08x} "
                              f"(recon/15: exporter sentinels, ignored)", []))


def rule_anchors(md, geom, entries, stats):
    lists = _git_lists(md.git, C.ANCHOR_LISTS)
    stats["anchors"] = {k: len(v) for k, v in lists.items()}
    hooks = md.lyt_hooks
    for label, items in lists.items():
        for d in items:
            pos = _pos_of(d)
            tag = d.get("Tag") or d.get("TemplateResRef") or "?"
            if pos is None:
                entries.append(_entry(C.R_ANCHOR, C.R_ANCHOR, "hard",
                                      f"{label} entry {tag!r} has no position fields",
                                      [label]))
                continue
            x, y, z = pos
            be = geom.bounds_excess(x, y)
            inside = be <= C.BOUNDS_TOL
            wd = geom.walk_dist(x, y)
            on_mesh = wd <= C.ON_WALKMESH_TOL
            if not inside and not on_mesh:
                entries.append(_entry(C.R_ANCHOR, C.R_ANCHOR, "hard",
                                      f"{label} {tag!r} at ({x:.3f},{y:.3f},{z:.3f}) is "
                                      f"{be:.3f} m outside area bounds and {wd:.3f} m off "
                                      f"the walkmesh", [label, str(tag)]))
            elif not on_mesh:
                entries.append(_entry(C.R_ANCHOR, C.T_ANCHOR_OFF_WALKMESH, "triage",
                                      f"{label} {tag!r} inside bounds but {wd:.3f} m off "
                                      f"walkmesh", [label, str(tag)]))
            if label == "Door List" and hooks:
                best_d, best_h = float("inf"), None
                for h in hooks:
                    dd = math.hypot(h["x"] - x, h["y"] - y)
                    if dd < best_d:
                        best_d, best_h = dd, h
                if best_d > C.HOOK_TOL_HARD:
                    entries.append(_entry(C.R_ANCHOR, C.R_ANCHOR, "hard",
                                          f"door {tag!r} has no LYT hook within "
                                          f"{C.HOOK_TOL_HARD} m (nearest {best_d:.3f} m)",
                                          ["Door List", str(tag)]))
                elif best_d > C.HOOK_TOL_TRIAGE:
                    entries.append(_entry(C.R_ANCHOR, C.T_DOOR_HOOK_OFFSET, "triage",
                                          f"door {tag!r} is {best_d * 1000:.1f} mm from its LYT "
                                          f"hook ({best_h['room']}/{best_h['node']})",
                                          ["Door List", str(tag)]))


# --------------------------------------------------------------------------

def validate_module(md) -> dict:
    entries = []
    stats = {}
    geom = AreaGeom(md)
    stats["rooms_lyt"] = len(md.lyt_rooms)
    stats["rooms_wok"] = len(md.woks)
    stats["hooks"] = len(md.lyt_hooks)
    stats["area_bounds"] = list(geom.bounds) if geom.bounds else None
    rule_winding(md, geom, entries, stats)
    rule_bounds(md, geom, entries, stats)
    rule_walkmesh(md, geom, entries, stats)
    rule_pth(md, geom, entries, stats)
    rule_lightmap_uv(md, geom, entries, stats)
    rule_anchors(md, geom, entries, stats)
    hard = [e for e in entries if e["severity"] == "hard"]
    triage = [e for e in entries if e["severity"] == "triage"]
    status = "hard_failed" if hard else ("triaged" if triage else "clean")
    return {
        "module": md.module,
        "area": md.area,
        "status": status,
        "hard_failures": len(hard),
        "triage": len(triage),
        "entries": entries,
        "stats": stats,
    }
