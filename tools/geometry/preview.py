#!/usr/bin/env python3
"""Render m01aa geometry with probe.py and emit SVG/JSON/Markdown.

Set KOTOR_AREA_DIR to the extracted area directory and KOTOR_PREVIEW_OUT to
the output directory (defaults to this tool directory). Read-only; no engine,
Ghidra or network use. Generated SVGs contain game-derived geometry and stay
local (artifacts/ is gitignored).
"""
from __future__ import annotations

import contextlib
import html
import io
import json
import math
import os
import re
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import probe  # noqa: E402  (local lane module; parses GFF/LYT/WOK/MDL)

BASE = probe.BASE
OUT_DIR = os.environ.get("KOTOR_PREVIEW_OUT", HERE)
os.makedirs(OUT_DIR, exist_ok=True)
SVG_PATH = os.path.join(OUT_DIR, "preview.svg")
STATS_PATH = os.path.join(OUT_DIR, "stats.json")
SUMMARY_PATH = os.path.join(OUT_DIR, "summary.md")

CANVAS_W, CANVAS_H = 1240, 1740
MAP_L, MAP_T = 92, 90
MAP_MAX_W, MAP_MAX_H = 512, 1568
WORLD_PAD = 2.0

PALETTE = [
    "#5b8ff9", "#f6bd16", "#5ad8a6", "#e8684a", "#6dc8ec", "#9270ca",
    "#ff9d4d", "#269a99", "#ff99c3", "#b6a2de", "#7ecf04", "#ffc069",
    "#69c0ff", "#d3adf7",
]


def r(v, n=6):
    v = round(float(v), n)
    return 0.0 if v == 0 else v


def bounds(points):
    if not points:
        return None
    return (
        min(p[0] for p in points), min(p[1] for p in points),
        min(p[2] for p in points) if len(points[0]) > 2 else None,
        max(p[0] for p in points), max(p[1] for p in points),
        max(p[2] for p in points) if len(points[0]) > 2 else None,
    )


def bbox_xyz(points):
    b = bounds(points)
    return None if b is None else [list(b[:3]), list(b[3:])]


def area2d(a, b, c):
    return abs((b[0] - a[0]) * (c[1] - a[1])
               - (b[1] - a[1]) * (c[0] - a[0])) * 0.5


def gff_list(g, label):
    for f in g.fields_of(0):
        if f.type == 15 and f.label == label:
            _, idx = g.list_of(f)
            return [g.as_dict(si) for si in idx]
    return []


def nearest_floor(floors_fn, px, py, pz):
    cand = floors_fn(px, py)
    if not cand:
        return None, 0
    cand.sort(key=lambda h: abs(h["z"] - pz))
    return cand[0], len(cand)


def gather():
    ifo = probe.gff.Gff(os.path.join(BASE, "m01aa.ifo"))
    git = probe.gff.Gff(os.path.join(BASE, "m01aa.git"))
    pth = probe.gff.Gff(os.path.join(BASE, "m01aa.pth"))
    ifo_dict = ifo.as_dict(0)
    entry = (ifo_dict["Mod_Entry_X"], ifo_dict["Mod_Entry_Y"],
             ifo_dict["Mod_Entry_Z"])
    edir = (ifo_dict["Mod_Entry_Dir_X"], ifo_dict["Mod_Entry_Dir_Y"])

    rooms_lyt, hooks = probe.parse_lyt(os.path.join(BASE, "m01aa.lyt"))
    woks, mdls, rooms = {}, {}, []
    bypos = defaultdict(set)
    for idx, r_lyt in enumerate(rooms_lyt):
        key = r_lyt["room"].lower()
        w = probe.Wok(os.path.join(BASE, key + ".wok"))
        m = probe.Mdl(os.path.join(BASE, key + ".mdl"))
        woks[key], mdls[key] = w, m
        for v in w.verts:
            bypos[(round(v[0], 4), round(v[1], 4), round(v[2], 4))].add(key)

        floor_faces, floor_area = [], 0.0
        tiny = tiny_1e4 = 0
        for fi, (f, n) in enumerate(zip(w.faces, w.normals)):
            a, b, c = (w.verts[i] for i in f)
            ar = area2d(a, b, c)
            if ar < 1e-6:
                tiny += 1
            if ar < 1e-4:
                tiny_1e4 += 1
            if n[2] > 0.5 and ar >= 1e-6:
                floor_faces.append(fi)
                floor_area += ar

        edge_count = Counter()
        for fi in floor_faces:
            f = w.faces[fi]
            for i, j in ((f[0], f[1]), (f[1], f[2]), (f[2], f[0])):
                edge_count[tuple(sorted((i, j)))] += 1

        # Area-weighted centroid of the rendered floor, for label placement.
        ta = cx = cy = 0.0
        for fi in floor_faces:
            a, b, c = (w.verts[i] for i in w.faces[fi])
            ar = area2d(a, b, c)
            ta += ar
            cx += ar * (a[0] + b[0] + c[0]) / 3.0
            cy += ar * (a[1] + b[1] + c[1]) / 3.0
        centroid = ((cx / ta, cy / ta) if ta else (r_lyt["x"], r_lyt["y"]))

        mdl_points, tri_nodes = [], 0
        for nd in m.nodes.values():
            if nd["type"] != 33:
                continue
            tri_nodes += 1
            Rm, t = probe.node_world(m, nd)
            for i in (0, 1):
                for j in (0, 1):
                    for k in (0, 1):
                        c = (nd["bbox_max"][0] if i else nd["bbox_min"][0],
                             nd["bbox_max"][1] if j else nd["bbox_min"][1],
                             nd["bbox_max"][2] if k else nd["bbox_min"][2])
                        mdl_points.append(tuple(
                            r_lyt["x" if q == 0 else "y" if q == 1 else "z"]
                            + t[q] + sum(Rm[q][s] * c[s] for s in range(3))
                            for q in range(3)))

        rooms.append({
            "name": r_lyt["room"], "key": key, "index": idx,
            "label": key.split("_")[-1],
            "origin": (r_lyt["x"], r_lyt["y"], r_lyt["z"]),
            "wok": w, "mdl": m,
            "wok_bbox": (bounds(w.verts) if w.verts else None),
            "floor_faces": floor_faces,
            "floor_boundary": [e for e, n in edge_count.items() if n == 1],
            "floor_area_m2": floor_area,
            "floor_z": ([min(w.verts[i][2] for f in floor_faces for i in w.faces[f]),
                         max(w.verts[i][2] for f in floor_faces for i in w.faces[f])]
                        if floor_faces else None),
            "centroid": centroid,
            "mdl_tri_nodes": tri_nodes,
            "mdl_bbox": bounds(mdl_points) if mdl_points else None,
            "tiny_triangles": tiny,
            "tiny_triangles_lt_1e-4": tiny_1e4,
        })

    def floors(px, py):
        out = []
        for room in rooms:
            w = room["wok"]
            for fi, f in enumerate(w.faces):
                tri = [(w.verts[i][0], w.verts[i][1]) for i in f]
                bc = probe.pip(px, py, tri)
                if bc is None:
                    continue
                out.append({
                    "room": room["name"], "face": fi, "bary": list(bc),
                    "z": sum(bc[k] * w.verts[f[k]][2] for k in range(3)),
                    "normal": list(w.normals[fi]) if w.normals else None,
                })
        return out

    # Shared vertex positions across rooms (probe.py uses 4-decimal keys).
    shared = {p: sorted(rr) for p, rr in bypos.items() if len(rr) > 1}
    pairs = Counter()
    for rr in shared.values():
        for i in range(len(rr)):
            for j in range(i + 1, len(rr)):
                pairs[(rr[i], rr[j])] += 1

    # Winding over every WOK face/normal pair.
    pos = neg = zero = 0
    min_dot = float("inf")
    nz_min, nz_max = 9.0, -9.0
    for room in rooms:
        w = room["wok"]
        for f, n in zip(w.faces, w.normals):
            a, b, c = (w.verts[i] for i in f)
            u = tuple(b[i] - a[i] for i in range(3))
            v = tuple(c[i] - a[i] for i in range(3))
            cr = (u[1] * v[2] - u[2] * v[1],
                  u[2] * v[0] - u[0] * v[2],
                  u[0] * v[1] - u[1] * v[0])
            dot = sum(cr[i] * n[i] for i in range(3))
            min_dot = min(min_dot, dot)
            if dot > 1e-6:
                pos += 1
            elif dot < -1e-6:
                neg += 1
            else:
                zero += 1
            nz_min, nz_max = min(nz_min, n[2]), max(nz_max, n[2])

    # LYT hook -> MDL node check.
    room_by_key = {room["key"]: room for room in rooms}
    hook_mdl = {}
    for h in hooks:
        key = h["room"].lower()
        mdl = mdls[key]
        room = room_by_key[key]
        nd = next((n for n in mdl.nodes.values()
                   if n["name"].lower() == h["node"].lower()), None)
        if nd is None:
            hook_mdl[(key, h["node"])] = {"found": False, "err": None,
                                          "world": None, "type": None,
                                          "depth": None}
            continue
        Rm, t = probe.node_world(mdl, nd)
        world = (room["origin"][0] + t[0], room["origin"][1] + t[1],
                 room["origin"][2] + t[2])
        hook_mdl[(key, h["node"])] = {
            "found": True, "world": world, "type": nd["type"],
            "depth": nd["depth"],
            "err": math.dist(world, (h["x"], h["y"], h["z"])),
        }

    creatures = gff_list(git, "Creature List")
    doors = gff_list(git, "Door List")
    waypoints = gff_list(git, "WaypointList")
    placeables = gff_list(git, "Placeable List")

    # Door -> nearest LYT hook, plane and MDL node distances.
    door_items = []
    used_hooks = set()
    for i, dd in enumerate(doors):
        h = min(hooks, key=lambda q: math.hypot(q["x"] - dd["X"],
                                                q["y"] - dd["Y"]))
        used_hooks.add((h["room"].lower(), h["node"]))
        xy = math.hypot(h["x"] - dd["X"], h["y"] - dd["Y"])
        dz = abs(h["z"] - dd["Z"])
        d3 = math.dist((h["x"], h["y"], h["z"]),
                       (dd["X"], dd["Y"], dd["Z"]))
        plane = abs(((math.degrees(dd["Bearing"]) - probe.qyaw(h["quat"])
                      + 90) % 180) - 90)
        hm = hook_mdl[(h["room"].lower(), h["node"])]
        door_mdl = (math.dist((dd["X"], dd["Y"], dd["Z"]), hm["world"])
                    if hm["found"] else None)
        door_items.append({
            "index": i, "tag": dd["Tag"],
            "template": dd["TemplateResRef"],
            "position": [dd["X"], dd["Y"], dd["Z"]],
            "bearing_deg": math.degrees(dd["Bearing"]),
            "hook_room": h["room"], "hook_node": h["node"],
            "xy_m": xy, "z_m": dz, "dist_m": d3, "plane_deg": plane,
            "hook_mdl_err_m": hm["err"],
            "door_mdl_dist_m": door_mdl,
            "hook_position": [h["x"], h["y"], h["z"]],
        })

    # PTH.
    pts_field = next(f for f in pth.fields_of(0) if f.label == "Path_Points")
    _, pids = pth.list_of(pts_field)
    point_dicts = [pth.as_dict(si) for si in pids]
    pts = [(p["X"], p["Y"]) for p in point_dicts]
    con_field = next(f for f in pth.fields_of(0)
                     if f.label == "Path_Conections")
    _, cids = pth.list_of(con_field)
    cons = [pth.as_dict(si)["Destination"] for si in cids]
    edges = []
    for u, p in enumerate(point_dicts):
        for q in range(p["Conections"]):
            edges.append((u, cons[p["First_Conection"] + q]))
    eset = set((min(u, v), max(u, v)) for u, v in edges)
    dset = set(edges)
    nonrecip = sum(1 for u, v in edges if (v, u) not in dset)
    adj = defaultdict(set)
    for u, v in edges:
        adj[u].add(v)
        adj[v].add(u)
    seen, comps = set(), []
    for i in range(len(pts)):
        if i in seen:
            continue
        stack, comp = [i], []
        seen.add(i)
        while stack:
            u = stack.pop()
            comp.append(u)
            for v in adj[u]:
                if v not in seen:
                    seen.add(v)
                    stack.append(v)
        comps.append(comp)
    comps.sort(key=len, reverse=True)
    zs, pth_multi, pth_missing = [], 0, 0
    for x, y in pts:
        cand = floors(x, y)
        if not cand:
            zs.append(None)
            pth_missing += 1
            continue
        if len(cand) > 1:
            pth_multi += 1
        cand.sort(key=lambda h: h["z"])
        zs.append(cand[0]["z"])
    pth_lens = [math.hypot(pts[u][0] - pts[v][0], pts[u][1] - pts[v][1])
                for u, v in edges]
    degrees = [len(adj[i]) for i in range(len(pts))]

    return {
        "ifo": ifo, "git": git, "pth": pth,
        "entry": entry, "entry_dir": edir,
        "rooms": rooms, "woks": woks, "mdls": mdls,
        "hooks": hooks, "hook_mdl": hook_mdl,
        "creatures": creatures, "doors": doors,
        "waypoints": waypoints, "placeables": placeables,
        "door_items": door_items, "used_hooks": used_hooks,
        "floors": floors,
        "shared": shared, "shared_pairs": pairs,
        "winding": {"positive": pos, "negative": neg, "zero": zero,
                    "min_dot": min_dot, "nz_min": nz_min, "nz_max": nz_max},
        "pth": {
            "points": pts, "connections": cons, "edges": edges,
            "undirected": eset, "nonrecip": nonrecip,
            "components": comps, "z": zs, "multi": pth_multi,
            "missing": pth_missing, "lens": pth_lens,
            "degrees": degrees,
        },
    }


def build_stats(d):
    rooms = d["rooms"]
    entry = d["entry"]
    entry_hits = d["floors"](entry[0], entry[1])
    for h in entry_hits:
        h["dz"] = entry[2] - h["z"]
    entry_hits.sort(key=lambda h: abs(h["dz"]))
    entry_floor = next((room for room in rooms
                        if entry_hits and room["name"] == entry_hits[0]["room"]),
                       None)
    entry_rendered = bool(entry_floor and entry_hits
                          and entry_hits[0]["face"] in entry_floor["floor_faces"])

    floor_triangles = sum(len(room["floor_faces"]) for room in rooms)
    non_upward = sum(1 for room in rooms for n in room["wok"].normals
                     if n[2] <= 0.5)
    tiny = sum(room["tiny_triangles"] for room in rooms)
    tiny_1e4 = sum(room["tiny_triangles_lt_1e-4"] for room in rooms)

    wok_all = [v for room in rooms for v in room["wok"].verts]
    floor_pts = [room["wok"].verts[i] for room in rooms
                 for fi in room["floor_faces"]
                 for i in room["wok"].faces[fi]]
    entity_pts = ([(entry[0], entry[1], entry[2])]
                  + [(c["XPosition"], c["YPosition"], c["ZPosition"])
                     for c in d["creatures"]]
                  + [(c["X"], c["Y"], c["Z"]) for c in d["doors"]]
                  + [(c["XPosition"], c["YPosition"], c["ZPosition"])
                     for c in d["waypoints"]]
                  + [(c["X"], c["Y"], c["Z"]) for c in d["placeables"]])
    pth_pts = [(x, y, z) for (x, y), z in zip(d["pth"]["points"], d["pth"]["z"])
               if z is not None]
    origins = [(room["origin"][0], room["origin"][1]) for room in rooms]
    wb = bounds(wok_all)
    eb = bounds(entity_pts)
    render = [wb[0] - WORLD_PAD, wb[1] - WORLD_PAD,
              wb[3] + WORLD_PAD, wb[4] + WORLD_PAD]
    outside = [room["name"] for room in rooms
               if not (render[0] <= room["origin"][0] <= render[2]
                       and render[1] <= room["origin"][1] <= render[3])]

    # Doors.
    door_items = d["door_items"]
    max_xy = max(item["xy_m"] for item in door_items)
    max_z = max(item["z_m"] for item in door_items)
    max_3d = max(item["dist_m"] for item in door_items)
    max_plane = max(item["plane_deg"] for item in door_items)
    max_door_mdl = max(item["door_mdl_dist_m"] for item in door_items
                       if item["door_mdl_dist_m"] is not None)
    ordered_doors = sorted(door_items, key=lambda item: -item["dist_m"])
    unused_hooks = [h for h in d["hooks"]
                    if (h["room"].lower(), h["node"]) not in d["used_hooks"]]

    # Hook -> MDL.
    hook_rows = []
    for h in d["hooks"]:
        hm = d["hook_mdl"][(h["room"].lower(), h["node"])]
        hook_rows.append({
            "room": h["room"], "node": h["node"],
            "found": hm["found"], "node_type": hm["type"],
            "err_m": hm["err"],
            "err_mm": hm["err"] * 1000.0 if hm["err"] is not None else None,
        })
    missing_hook_nodes = [r for r in hook_rows if not r["found"]]
    max_hook_mdl = max(r["err_m"] for r in hook_rows if r["found"])

    # Floor conformance for GIT entities.
    def delta_rows(items, xyz, name_fn):
        rows = []
        for i, item in enumerate(items):
            x, y, z = xyz(item)
            h, n = nearest_floor(d["floors"], x, y, z)
            rows.append({
                "name": name_fn(item, i), "index": i,
                "x": x, "y": y, "z": z,
                "floor_z": h["z"] if h else None,
                "floor_room": h["room"] if h else None,
                "floor_face": h["face"] if h else None,
                "delta_m": (z - h["z"]) if h else None,
                "candidates": n,
            })
        return rows

    creature_rows = delta_rows(
        d["creatures"], lambda c: (c["XPosition"], c["YPosition"], c["ZPosition"]),
        lambda c, i: c["TemplateResRef"])
    waypoint_rows = delta_rows(
        d["waypoints"], lambda c: (c["XPosition"], c["YPosition"], c["ZPosition"]),
        lambda c, i: c["Tag"])
    placeable_rows = delta_rows(
        d["placeables"], lambda c: (c["X"], c["Y"], c["Z"]),
        lambda c, i: "%s#%d" % (c["TemplateResRef"], i))

    def delta_summary(rows, threshold):
        present = [x for x in rows if x["delta_m"] is not None]
        worst = sorted(present, key=lambda x: -abs(x["delta_m"]))[:8]
        return {
            "n": len(rows), "hits": len(present),
            "misses": len(rows) - len(present),
            "max_abs_m": (max(abs(x["delta_m"]) for x in present)
                          if present else None),
            "within_threshold": sum(1 for x in present
                                    if abs(x["delta_m"]) < threshold),
            "threshold_m": threshold,
            "multiple_floor_candidates": sum(1 for x in rows
                                             if x["candidates"] > 1),
            "worst": [{"name": x["name"], "delta_m": x["delta_m"],
                       "floor_room": x["floor_room"],
                       "floor_face": x["floor_face"],
                       "candidates": x["candidates"]} for x in worst],
        }

    creature_sum = delta_summary(creature_rows, 0.01)
    waypoint_sum = delta_summary(waypoint_rows, 0.01)
    placeable_sum = delta_summary(placeable_rows, 0.05)

    # Orientation checks (same computations as probe.py).
    ori_err = max(abs(math.hypot(c["XOrientation"], c["YOrientation"]) - 1)
                  for c in d["creatures"])
    cgrid = max(abs(math.degrees(math.atan2(c["YOrientation"], c["XOrientation"]))
                    - round(math.degrees(math.atan2(c["YOrientation"],
                                                    c["XOrientation"])) / 11.25)
                    * 11.25)
                for c in d["creatures"])
    bdeg = [math.degrees(c["Bearing"]) for c in d["placeables"]]
    bgrid = max(abs(a - round(a / 11.25) * 11.25) for a in bdeg)

    # MDL bbox containment of WOK XY (probe's weak bbox check).
    mdl_rows = []
    for room in rooms:
        wb_room, mb = room["wok_bbox"], room["mdl_bbox"]
        if room["key"] == "m01aa_07b":
            mdl_rows.append({"room": room["name"], "contains": None,
                             "deficit_mm": None,
                             "mdl_trimesh_nodes": room["mdl_tri_nodes"],
                             "reason": "no WOK vertices; skybox-scale MDL bbox"})
            continue
        if wb_room is None or mb is None:
            mdl_rows.append({"room": room["name"], "contains": None,
                             "deficit_mm": None,
                             "mdl_trimesh_nodes": room["mdl_tri_nodes"],
                             "reason": "no type-33 MDL trimesh"})
            continue
        contains = (mb[0] <= wb_room[0] and mb[1] <= wb_room[1]
                    and mb[3] >= wb_room[3] and mb[4] >= wb_room[4])
        deficit = max(0.0, mb[0] - wb_room[0], mb[1] - wb_room[1],
                      wb_room[3] - mb[3], wb_room[4] - mb[4])
        mdl_rows.append({"room": room["name"], "contains": contains,
                         "deficit_mm": deficit * 1000.0,
                         "mdl_trimesh_nodes": room["mdl_tri_nodes"],
                         "reason": None})
    true_mdl = [x["room"] for x in mdl_rows if x["contains"] is True]
    false_mdl = [x for x in mdl_rows if x["contains"] is False]
    na_mdl = [x["room"] for x in mdl_rows if x["contains"] is None]
    max_deficit = max((x["deficit_mm"] for x in false_mdl), default=0.0)

    # PTH.
    p = d["pth"]
    pz = [z for z in p["z"] if z is not None]
    pth_stats = {
        "points": len(p["points"]),
        "connections": len(p["connections"]),
        "undirected_edges": len(p["undirected"]),
        "duplicate_directions": len(p["edges"]) - len(p["undirected"]),
        "non_reciprocal": p["nonrecip"],
        "dest_range": [min(p["connections"]), max(p["connections"])],
        "bad_destinations": sum(1 for x in p["connections"]
                                if x < 0 or x >= len(p["points"])),
        "components": {"count": len(p["components"]),
                       "sizes": [len(c) for c in p["components"]]},
        "degree": {"min": min(p["degrees"]), "max": max(p["degrees"]),
                   "mean": sum(p["degrees"]) / len(p["degrees"])},
        "edge_length_m": {"min": min(p["lens"]), "max": max(p["lens"]),
                          "mean": sum(p["lens"]) / len(p["lens"])},
        "floor_z": {"min": min(pz), "max": max(pz),
                    "multiple_candidates": p["multi"],
                    "missing": p["missing"]},
        "bounds_xy": [[min(x for x, _ in p["points"]), min(y for _, y in p["points"])],
                      [max(x for x, _ in p["points"]), max(y for _, y in p["points"])]],
    }

    # Shared vertices.
    pair_rows = [{"rooms": [a, b], "positions": n}
                 for (a, b), n in sorted(d["shared_pairs"].items(),
                                         key=lambda kv: (-kv[1], kv[0]))]

    door_max_item = ordered_doors[0]
    other_doors_max = max(item["dist_m"] for item in door_items
                          if item is not door_max_item)
    wp_worst = sorted([x for x in waypoint_rows if x["delta_m"] is not None],
                      key=lambda x: -abs(x["delta_m"]))
    pl_worst = sorted([x for x in placeable_rows if x["delta_m"] is not None],
                      key=lambda x: -abs(x["delta_m"]))
    wp_over = [x for x in wp_worst if abs(x["delta_m"]) >= 0.01]
    pl_over = [x for x in pl_worst if abs(x["delta_m"]) >= 0.05]

    # Not-rendered / caveat entries.
    room_07b = next(room for room in rooms if room["key"] == "m01aa_07b")
    room_08d = next(room for room in rooms if room["key"] == "m01aa_08d")
    b7 = room_07b["mdl_bbox"]
    dims7 = ([b7[3] - b7[0], b7[4] - b7[1], b7[5] - b7[2]] if b7 else None)

    stats = {
        "area": "m01aa",
        "generated_by": "tools/geometry/preview.py",
        "source_dir": probe.BASE,
        "counts": {
            "lyt_rooms": len(rooms),
            "rooms_with_wok_floor": sum(1 for room in rooms
                                        if room["floor_faces"]),
            "lyt_door_hooks": len(d["hooks"]),
            "wok_vertices": len(wok_all),
            "wok_triangles": sum(len(room["wok"].faces) for room in rooms),
            "wok_floor_triangles": floor_triangles,
            "git": {"creatures": len(d["creatures"]), "doors": len(d["doors"]),
                    "waypoints": len(d["waypoints"]),
                    "placeables": len(d["placeables"])},
            "pth": {"points": pth_stats["points"],
                    "connections": pth_stats["connections"],
                    "undirected_edges": pth_stats["undirected_edges"],
                    "components": pth_stats["components"]["count"]},
        },
        "bounds_m": {
            "wok_xyz": bbox_xyz(wok_all),
            "wok_floor_xyz": bbox_xyz(floor_pts),
            "entities_xyz": bbox_xyz(entity_pts),
            "pth_floor_xyz": bbox_xyz(pth_pts),
            "lyt_origins_xy": [[min(x for x, _ in origins),
                                min(y for _, y in origins)],
                               [max(x for x, _ in origins),
                                max(y for _, y in origins)]],
            "lyt_origins_outside_render": outside,
            "render_xy": render,
        },
        "entry": {
            "position": list(entry), "direction_xy": list(d["entry_dir"]),
            "direction_norm_error": abs(math.hypot(*d["entry_dir"]) - 1.0),
            "floor_hits": [{"room": h["room"], "face": h["face"],
                            "z": h["z"], "dz": h["dz"],
                            "bary": h["bary"], "normal": h["normal"]}
                           for h in entry_hits],
            "hit": bool(entry_hits),
            "rendered_wok_floor_triangle": entry_rendered,
            "floor_room": entry_hits[0]["room"] if entry_hits else None,
            "floor_face": entry_hits[0]["face"] if entry_hits else None,
            "dz_m": entry_hits[0]["dz"] if entry_hits else None,
        },
        "wok": {
            "winding": {"positive": d["winding"]["positive"],
                        "negative": d["winding"]["negative"],
                        "zero": d["winding"]["zero"],
                        "min_dot": d["winding"]["min_dot"]},
            "normal_z_range": [d["winding"]["nz_min"], d["winding"]["nz_max"]],
            "floor_filter": {
                "normal_z_gt": 0.5, "min_projected_area_m2": 1e-6,
                "drawn": floor_triangles, "non_upward": non_upward,
                "degenerate_lt_1e-6_m2": tiny,
                "degenerate_lt_1e-4_m2": tiny_1e4,
            },
            "shared_vertices": {"positions": len(d["shared"]),
                                "room_pairs": pair_rows},
        },
        "doors": {
            "door_count": len(door_items),
            "max_xy_mm": max_xy * 1000.0,
            "max_z_mm": max_z * 1000.0,
            "max_3d_mm": max_3d * 1000.0,
            "max_plane_deg": max_plane,
            "max_to_mdl_node_mm": max_door_mdl * 1000.0,
            "hooks_used": len(d["used_hooks"]),
            "unused_hooks": [{"room": h["room"], "node": h["node"]}
                             for h in unused_hooks],
            "items": [{
                "index": item["index"], "tag": item["tag"],
                "template": item["template"],
                "position": item["position"],
                "hook_room": item["hook_room"], "hook_node": item["hook_node"],
                "xy_mm": item["xy_m"] * 1000.0,
                "z_mm": item["z_m"] * 1000.0,
                "dist_mm": item["dist_m"] * 1000.0,
                "plane_deg": item["plane_deg"],
                "hook_mdl_err_mm": (item["hook_mdl_err_m"] * 1000.0
                                    if item["hook_mdl_err_m"] is not None else None),
                "door_mdl_node_dist_mm": (item["door_mdl_dist_m"] * 1000.0
                                          if item["door_mdl_dist_m"] is not None
                                          else None),
            } for item in door_items],
        },
        "hook_to_mdl": {
            "hook_count": len(hook_rows),
            "found": len(hook_rows) - len(missing_hook_nodes),
            "missing": missing_hook_nodes,
            "max_err_mm": max_hook_mdl * 1000.0,
            "items": [{"room": x["room"], "node": x["node"],
                       "node_type": x["node_type"],
                       "err_mm": x["err_mm"]} for x in hook_rows],
        },
        "floor_conformance": {
            "creatures": creature_sum,
            "waypoints": waypoint_sum,
            "placeables": placeable_sum,
        },
        "orientation": {
            "creature_norm_error_max": ori_err,
            "creature_grid_dev_max_deg": cgrid,
            "placeable_bearing_grid_dev_max_deg": bgrid,
            "placeable_bearing_deg_range": [min(bdeg), max(bdeg)],
        },
        "pth": pth_stats,
        "mdl_bbox_contains_wok_xy": {
            "true": true_mdl, "false": [x["room"] for x in false_mdl],
            "not_applicable": na_mdl,
            "max_false_deficit_mm": max_deficit,
            "semantics_note": "probe.py prints False for M01aa_08d because it "
                              "has no type-33 MDL trimesh; this preview reports "
                              "that case as not_applicable. probe.py excludes "
                              "M01aa_07b by name.",
            "items": mdl_rows,
        },
        "probe_parity": None,
        "discrepancies": [
            {"kind": "door_hook_offset",
             "where": "%s -> %s/%s" % (door_max_item["tag"],
                                       door_max_item["hook_room"],
                                       door_max_item["hook_node"]),
             "value_mm": door_max_item["dist_m"] * 1000.0,
             "other_doors_max_mm": other_doors_max * 1000.0,
             "note": "GIT door position differs from the LYT hook; that hook "
                     "still matches its MDL node to %.3f mm."
                     % (door_max_item["hook_mdl_err_m"] * 1000.0)},
            {"kind": "waypoint_floor_offsets",
             "where": wp_worst[0]["name"] if wp_worst else None,
             "value_m": abs(wp_worst[0]["delta_m"]) if wp_worst else None,
             "over_1cm": len(wp_over),
             "note": "Second: %s +%.3f m." % (
                 wp_worst[1]["name"] if len(wp_worst) > 1 else "n/a",
                 abs(wp_worst[1]["delta_m"]) if len(wp_worst) > 1 else 0.0)},
            {"kind": "placeable_floor_offsets",
             "where": [x["name"] for x in pl_over],
             "value_m": abs(pl_worst[0]["delta_m"]) if pl_worst else None,
             "over_5cm": len(pl_over),
             "note": "Elevated placeables; no conclusion about intent."},
            {"kind": "room_not_rendered", "where": room_07b["name"],
             "note": "0 WOK vertices (no floor), LYT origin (%0.3f, %0.3f) is "
                     "outside the map, MDL bbox is %s m (skybox-scale)."
                     % (room_07b["origin"][0], room_07b["origin"][1],
                        " x ".join("%.1f" % v for v in dims7) if dims7 else "n/a")},
            {"kind": "mdl_geometry_missing", "where": room_08d["name"],
             "note": "4 WOK vertices / 2 floor triangles but no type-33 MDL "
                     "trimesh, so the MDL bbox check is not applicable."},
            {"kind": "mdl_bbox_miss",
             "where": [x["room"] for x in false_mdl],
             "max_deficit_mm": max_deficit,
             "note": "MDL XY bbox does not fully contain WOK XY; deficits "
                     "range from sub-mm to the value shown."},
            {"kind": "wok_non_floor_faces",
             "count": non_upward,
             "note": "WOK side/wall or downward faces are not filled; only "
                     "upward faces (n.z > 0.5) are drawn as floor."},
        ],
        "rendering": {
            "svg": "preview.svg",
            "canvas_px": [CANVAS_W, CANVAS_H],
            "scale_px_per_m": None,
            "world_window_xy": render,
            "floor_triangles_drawn": floor_triangles,
            "floor_rooms_drawn": sum(1 for room in rooms if room["floor_faces"]),
            "room_outlines": "WOK floor boundary edges (used once); LYT "
                             "provides room origins only",
            "room_labels": "placed at WOK floor centroids; LYT origin marked "
                           "with a cross",
            "not_drawn": [
                {"item": room_07b["name"],
                 "reason": "no WOK vertices/floor; LYT origin off-map; MDL "
                           "skybox-scale bbox"},
                {"item": "WOK non-floor faces",
                 "reason": "%d side/downward faces omitted from floor fill"
                           % non_upward},
            ],
        },
    }
    return stats, {
        "entry_hits": entry_hits, "max_xy": max_xy, "max_z": max_z,
        "max_3d": max_3d, "max_plane": max_plane, "max_hook_mdl": max_hook_mdl,
        "creature_max": creature_sum["max_abs_m"],
        "waypoint_max": waypoint_sum["max_abs_m"],
        "waypoint_within": waypoint_sum["within_threshold"],
        "placeable_max": placeable_sum["max_abs_m"],
        "placeable_within": placeable_sum["within_threshold"],
        "mdl_true": len(true_mdl), "mdl_false": len(false_mdl),
        "mdl_na": len(na_mdl), "max_deficit": max_deficit,
    }


def verify_probe_parity(stats, values):
    """Run probe.main() and compare the headline numbers printed by probe.py."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        probe.main()
    out = buf.getvalue()

    def find(pattern, flags=0):
        m = re.search(pattern, out, flags)
        if not m:
            raise RuntimeError("probe.py output missing pattern: %s" % pattern)
        return m

    checks = []

    def eq(name, expected, actual, tol=0.0):
        ok = (abs(float(expected) - float(actual)) <= tol
              if tol else expected == actual)
        checks.append({"name": name, "ok": ok,
                       "probe": expected, "stats": actual})
        if not ok:
            raise RuntimeError("probe parity failed: %s probe=%r stats=%r"
                               % (name, expected, actual))

    m = find(r"rooms (\d+) hooks (\d+)")
    eq("lyt_rooms", int(m.group(1)), stats["counts"]["lyt_rooms"])
    eq("door_hooks", int(m.group(2)), stats["counts"]["lyt_door_hooks"])

    m = find(r"counts (\d+) (\d+) (\d+) (\d+)")
    for i, key in enumerate(("creatures", "doors", "waypoints", "placeables")):
        eq("git_" + key, int(m.group(i + 1)),
           stats["counts"]["git"][key])

    m = find(r"winding \+(\d+) -(\d+) 0(\d+)")
    eq("winding_pos", int(m.group(1)), stats["wok"]["winding"]["positive"])
    eq("winding_neg", int(m.group(2)), stats["wok"]["winding"]["negative"])
    eq("winding_zero", int(m.group(3)), stats["wok"]["winding"]["zero"])

    m = find(r"entry floors:\n   (M01aa_\w+) (\d+) ([-0-9.]+) dz ([-0-9.e+]+)")
    entry_hit = stats["entry"]["floor_hits"][0]
    eq("entry_floor_room", m.group(1), entry_hit["room"])
    eq("entry_floor_face", int(m.group(2)), entry_hit["face"])
    eq("entry_floor_z", float(m.group(3)), entry_hit["z"], 1e-6)
    eq("entry_dz", float(m.group(4)), entry_hit["dz"], 1e-6)

    m = find(r"max_xy ([-0-9.e+]+) max_z ([-0-9.e+]+) max_3d "
             r"([-0-9.e+]+) unique hooks (\d+)")
    eq("door_max_xy_m", float(m.group(1)), values["max_xy"], 1e-12)
    eq("door_max_z_m", float(m.group(2)), values["max_z"], 1e-12)
    eq("door_max_3d_m", float(m.group(3)), values["max_3d"], 1e-12)
    eq("hooks_used", int(m.group(4)), stats["doors"]["hooks_used"])

    m = find(r"LYT hook vs MDL node:([\s\S]*?)\n  max ([-0-9.e+]+)")
    eq("hook_to_mdl_max_m", float(m.group(2)), values["max_hook_mdl"], 1e-12)

    eq("shared_vertices", int(find(r"shared vertex positions (\d+)").group(1)),
       stats["wok"]["shared_vertices"]["positions"])

    m = find(r"pth pts (\d+) cons (\d+) dest range (\d+) (\d+) bad (\d+)")
    eq("pth_points", int(m.group(1)), stats["pth"]["points"])
    eq("pth_connections", int(m.group(2)), stats["pth"]["connections"])
    eq("pth_bad_destinations", int(m.group(5)),
       stats["pth"]["bad_destinations"])

    m = find(r"components (\d+) \[([\d, ]+)\]")
    eq("pth_components", int(m.group(1)), stats["pth"]["components"]["count"])

    m = find(r"unique undirected (\d+) dups (\d+)")
    eq("pth_undirected", int(m.group(1)), stats["pth"]["undirected_edges"])
    eq("pth_duplicate_directions", int(m.group(2)),
       stats["pth"]["duplicate_directions"])
    eq("pth_non_reciprocal",
       int(find(r"non-reciprocal (\d+)").group(1)),
       stats["pth"]["non_reciprocal"])

    m = find(r"pth z range ([-0-9.e+]+) ([-0-9.e+]+) multi (\d+) missing (\d+)")
    eq("pth_z_min", float(m.group(1)), stats["pth"]["floor_z"]["min"], 1e-6)
    eq("pth_z_max", float(m.group(2)), stats["pth"]["floor_z"]["max"], 1e-6)
    eq("pth_multi", int(m.group(3)),
       stats["pth"]["floor_z"]["multiple_candidates"])
    eq("pth_missing", int(m.group(4)), stats["pth"]["floor_z"]["missing"])

    m = find(r"edge len min/max/mean ([-0-9.e+]+) ([-0-9.e+]+) ([-0-9.e+]+)")
    eq("pth_edge_min", float(m.group(1)),
       stats["pth"]["edge_length_m"]["min"], 1e-9)
    eq("pth_edge_max", float(m.group(2)),
       stats["pth"]["edge_length_m"]["max"], 1e-9)
    eq("pth_edge_mean", float(m.group(3)),
       stats["pth"]["edge_length_m"]["mean"], 1e-9)

    mdl_true_probe = len(re.findall(r"mdl_contains_wok_xy=True", out))
    mdl_false_probe = len(re.findall(r"mdl_contains_wok_xy=False", out))
    eq("mdl_bbox_true", mdl_true_probe, values["mdl_true"])
    # probe.py prints False when there is no MDL trimesh (M01aa_08d); this
    # preview reports that case as n/a rather than as a bbox failure.
    na_as_false = sum(
        1 for item in stats["mdl_bbox_contains_wok_xy"]["items"]
        if item["contains"] is None and item["room"] != "M01aa_07b")
    eq("mdl_bbox_false", mdl_false_probe, values["mdl_false"] + na_as_false)

    m = find(r"creature floors:\n  n (\d+) max abs ([-0-9.e+]+)")
    eq("creature_n", int(m.group(1)),
       stats["floor_conformance"]["creatures"]["n"])
    eq("creature_max", float(m.group(2)), values["creature_max"], 1e-12)

    m = find(r"waypoint floors:\n  n (\d+) miss (\d+) max abs "
             r"([-0-9.e+]+) within 1cm (\d+)")
    eq("waypoint_n", int(m.group(1)),
       stats["floor_conformance"]["waypoints"]["n"])
    eq("waypoint_miss", int(m.group(2)),
       stats["floor_conformance"]["waypoints"]["misses"])
    eq("waypoint_max", float(m.group(3)), values["waypoint_max"], 1e-12)
    eq("waypoint_within_1cm", int(m.group(4)), values["waypoint_within"])

    m = find(r"placeable floors:\n  n (\d+) miss (\d+) within 5cm (\d+) "
             r"max ([-0-9.e+]+)")
    eq("placeable_n", int(m.group(1)),
       stats["floor_conformance"]["placeables"]["n"])
    eq("placeable_miss", int(m.group(2)),
       stats["floor_conformance"]["placeables"]["misses"])
    eq("placeable_within_5cm", int(m.group(3)), values["placeable_within"])
    eq("placeable_max", float(m.group(4)), values["placeable_max"], 1e-12)

    return {"ok": all(x["ok"] for x in checks),
            "checks": len(checks),
            "source": "probe.py stdout (same extracted assets)",
            "items": checks}


def wrap(text, width):
    words, lines, line = text.split(), [], ""
    for w in words:
        if len(line) + len(w) + (1 if line else 0) <= width:
            line = (line + " " + w) if line else w
        else:
            lines.append(line)
            line = w
    if line:
        lines.append(line)
    return lines


def build_svg(d, stats, values):
    rooms = d["rooms"]
    wp_worst = stats["floor_conformance"]["waypoints"]["worst"]
    false_mdl = [x for x in stats["mdl_bbox_contains_wok_xy"]["items"]
                 if x["contains"] is False]
    room_07b = next(room for room in rooms if room["key"] == "m01aa_07b")
    b7 = room_07b["mdl_bbox"]
    dims7 = ([b7[3] - b7[0], b7[4] - b7[1], b7[5] - b7[2]] if b7 else None)
    render = stats["bounds_m"]["render_xy"]
    wx0, wy0, wx1, wy1 = render
    scale = min(MAP_MAX_W / (wx1 - wx0), MAP_MAX_H / (wy1 - wy0))
    map_w = (wx1 - wx0) * scale
    map_h = (wy1 - wy0) * scale
    map_r = MAP_L + map_w
    map_b = MAP_T + map_h
    panel_x = map_r + 42
    panel_w = CANVAS_W - panel_x - 26
    stats["rendering"]["scale_px_per_m"] = r(scale, 4)

    def sx(x):
        return MAP_L + (x - wx0) * scale

    def sy(y):
        return MAP_T + (wy1 - y) * scale

    def pt(x, y):
        return sx(x), sy(y)

    def fmt(v):
        s = "%.2f" % v
        return "0.00" if s == "-0.00" else s

    def text(x, y, s, cls="", anchor=None):
        a = ' text-anchor="%s"' % anchor if anchor else ""
        return ('<text x="%s" y="%s" class="%s"%s>%s</text>'
                % (fmt(x), fmt(y), cls, a, html.escape(str(s))))

    out = []
    out.append('<?xml version="1.0" encoding="UTF-8"?>')
    out.append('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
               'viewBox="0 0 %d %d" role="img" aria-labelledby="ttl desc">'
               % (CANVAS_W, CANVAS_H, CANVAS_W, CANVAS_H))
    out.append('<title id="ttl">m01aa geometry preview</title>')
    out.append('<desc id="desc">Top-down preview of the m01aa walkmesh, LYT '
               'rooms, door hooks, GIT entities, PTH graph and IFO entry.'
               '</desc>')
    out.append("""<style>
  text { font-family: "Segoe UI", Arial, Helvetica, sans-serif; fill: #1b2430; }
  .grid-minor { stroke: #eef1f5; stroke-width: 0.7; }
  .grid-major { stroke: #dde4ec; stroke-width: 0.9; }
  .tick { font-size: 9px; fill: #667382; }
  .axis { stroke: #596675; stroke-width: 1.2; fill: none; }
  .axis-label { font-size: 10px; fill: #596675; }
  .floor { fill-opacity: 0.22; }
  .wout { fill: none; stroke: #33414f; stroke-opacity: 0.72; stroke-width: 1.15; }
  .rlabel-bg { fill: #ffffff; fill-opacity: 0.84; stroke: #c7d0da; stroke-width: 0.5; }
  .rlabel { font-size: 10.5px; font-weight: 600; fill: #25313e; }
  .origin { stroke: #8a95a2; stroke-width: 1; fill: none; }
  .plink { fill: none; stroke: #3c4858; stroke-opacity: 0.55; stroke-width: 1.15; }
  .pnode { fill: #ffffff; stroke: #26303c; stroke-width: 0.75; }
  .hook { fill: #c2185b; stroke: #760d39; stroke-width: 0.9; }
  .hooktick { stroke: #c2185b; stroke-width: 1.05; fill: none; }
  .door { fill: none; stroke: #7b1fa2; stroke-width: 1.25; }
  .creature { fill: #1976d2; stroke: #0d47a1; stroke-width: 0.9; }
  .placeable { fill: #2e7d32; stroke: #1b5e20; stroke-width: 0.8; }
  .waypoint { fill: #ef6c00; stroke: #a63d00; stroke-width: 0.85; }
  .entrystar { fill: #e53935; stroke: #7f0000; stroke-width: 1.1; }
  .entryarrow { stroke: #e53935; stroke-width: 2; fill: none; }
  .entryfloor { fill: #ffd54f; fill-opacity: 0.14; stroke: #f9a825; stroke-width: 2.2; }
  .entrylabel { font-size: 10.5px; font-weight: 600; fill: #b71c1c;
                paint-order: stroke; stroke: #ffffff; stroke-width: 3px; }
  .ring-floor { fill: none; stroke: #d32f2f; stroke-width: 1.25;
                stroke-dasharray: 2.5 2.5; }
  .ring-door { fill: none; stroke: #f9a825; stroke-width: 1.35;
               stroke-dasharray: 3 2; }
  .note { font-size: 10px; fill: #52606d; paint-order: stroke;
          stroke: #ffffff; stroke-width: 3px; }
  .offmap { font-size: 10.5px; fill: #7a4b00; paint-order: stroke;
            stroke: #ffffff; stroke-width: 3px; }
  .h1 { font-size: 23px; font-weight: 700; fill: #10263d; }
  .sub { font-size: 11.5px; fill: #5a6878; }
  .sec { font-size: 12.5px; font-weight: 700; fill: #16324f; letter-spacing: 0.6px; }
  .item { font-size: 11.5px; fill: #26313d; }
  .mono { font-family: Consolas, "Cascadia Mono", monospace; font-size: 10.7px;
          fill: #26313d; }
  .warn { font-size: 11px; fill: #8a2c2c; }
  .rule { stroke: #d4dce6; stroke-width: 1; }
  .panel { fill: #f8fafc; stroke: #d5dde7; }
  .swatch { stroke: #ffffff; stroke-width: 0.5; }
</style>""")

    out.append('<rect x="0" y="0" width="%d" height="%d" fill="#ffffff"/>'
               % (CANVAS_W, CANVAS_H))

    # Map background and grid.
    out.append('<rect x="%s" y="%s" width="%s" height="%s" fill="#fcfcfd" '
               'stroke="#c6ced8" stroke-width="1"/>'
               % (fmt(MAP_L), fmt(MAP_T), fmt(map_w), fmt(map_h)))
    grid = ['<g id="grid">']
    step = 5
    x = math.ceil(wx0 / step) * step
    while x <= wx1:
        major = abs(x % 10) < 1e-9
        X = sx(x)
        grid.append('<line x1="%s" y1="%s" x2="%s" y2="%s" class="%s"/>'
                    % (fmt(X), fmt(MAP_T), fmt(X), fmt(map_b),
                       "grid-major" if major else "grid-minor"))
        if major:
            grid.append(text(X, MAP_T - 6, "%d" % round(x), "tick", "middle"))
        x += step
    y = math.ceil(wy0 / step) * step
    while y <= wy1:
        major = abs(y % 10) < 1e-9
        Y = sy(y)
        grid.append('<line x1="%s" y1="%s" x2="%s" y2="%s" class="%s"/>'
                    % (fmt(MAP_L), fmt(Y), fmt(map_r), fmt(Y),
                       "grid-major" if major else "grid-minor"))
        if major:
            grid.append(text(MAP_L - 7, Y + 3, "%d" % round(y), "tick", "end"))
        y += step
    grid.append("</g>")
    out.extend(grid)

    # Axes outside the map.
    ax = ['<g id="axes">']
    ax.append('<line x1="%s" y1="32" x2="%s" y2="32" class="axis"/>'
              % (fmt(MAP_L), fmt(MAP_L + 64)))
    ax.append('<path d="M %s 26 L %s 32 L %s 38 z" fill="#596675"/>'
              % (fmt(MAP_L + 64), fmt(MAP_L + 72), fmt(MAP_L + 64)))
    ax.append(text(MAP_L + 78, 36, "X+ (m)", "axis-label"))
    ax.append('<line x1="%s" y1="%s" x2="%s" y2="%s" class="axis"/>'
              % (fmt(MAP_L - 55), fmt(MAP_T + 64), fmt(MAP_L - 55), fmt(MAP_T)))
    ax.append('<path d="M %s %s L %s %s L %s %s z" fill="#596675"/>'
              % (fmt(MAP_L - 61), fmt(MAP_T), fmt(MAP_L - 55), fmt(MAP_T - 8),
                 fmt(MAP_L - 49), fmt(MAP_T)))
    ax.append('<g transform="translate(%s %s) rotate(-90)">%s</g>'
              % (fmt(MAP_L - 60), fmt(MAP_T + 70),
                 text(0, 0, "Y+ (m)", "axis-label")))
    ax.append("</g>")
    out.extend(ax)

    # WOK floor fills (upward-facing, non-degenerate) and floor boundary.
    out.append('<g id="wok">')
    for i, room in enumerate(rooms):
        if not room["floor_faces"]:
            continue
        w = room["wok"]
        color = PALETTE[i % len(PALETTE)]
        segs = []
        for fi in room["floor_faces"]:
            a, b, c = (w.verts[j] for j in w.faces[fi])
            xa, ya = pt(a[0], a[1])
            xb, yb = pt(b[0], b[1])
            xc, yc = pt(c[0], c[1])
            segs.append("M %s %s L %s %s L %s %s Z"
                        % (fmt(xa), fmt(ya), fmt(xb), fmt(yb),
                           fmt(xc), fmt(yc)))
        out.append('<path class="floor" fill="%s" data-room="%s" d="%s"/>'
                   % (color, room["key"], " ".join(segs)))
    for room in rooms:
        if not room["floor_boundary"]:
            continue
        w = room["wok"]
        segs = []
        for i, j in room["floor_boundary"]:
            xa, ya = pt(w.verts[i][0], w.verts[i][1])
            xb, yb = pt(w.verts[j][0], w.verts[j][1])
            segs.append("M %s %s L %s %s"
                        % (fmt(xa), fmt(ya), fmt(xb), fmt(yb)))
        out.append('<path class="wout" data-room="%s" d="%s"/>'
                   % (room["key"], " ".join(segs)))
    out.append("</g>")

    # Entry floor triangle highlight (verified hit).
    if stats["entry"]["hit"]:
        hit = stats["entry"]["floor_hits"][0]
        room = next(room for room in rooms if room["name"] == hit["room"])
        w = room["wok"]
        f = w.faces[hit["face"]]
        pts = [pt(w.verts[i][0], w.verts[i][1]) for i in f]
        out.append('<polygon class="entryfloor" points="%s"/>'
                   % " ".join("%s,%s" % (fmt(x), fmt(y)) for x, y in pts))

    # PTH links (undirected) and nodes.
    out.append('<g id="pth">')
    segs = []
    for u, v in sorted(d["pth"]["undirected"]):
        x1, y1 = pt(*d["pth"]["points"][u])
        x2, y2 = pt(*d["pth"]["points"][v])
        segs.append("M %s %s L %s %s" % (fmt(x1), fmt(y1), fmt(x2), fmt(y2)))
    out.append('<path class="plink" d="%s"/>' % " ".join(segs))
    for x, y in d["pth"]["points"]:
        X, Y = pt(x, y)
        out.append('<circle class="pnode" cx="%s" cy="%s" r="1.8"/>'
                   % (fmt(X), fmt(Y)))
    out.append("</g>")

    # LYT origins and room labels.
    for room in rooms:
        if not room["floor_faces"]:
            continue
        ox, oy = pt(room["origin"][0], room["origin"][1])
        out.append('<path class="origin" d="M %s %s l -2.5 0 M %s %s l 0 -2.5" '
                   'clip-path="none"/>'
                   % (fmt(ox), fmt(oy), fmt(ox), fmt(oy)))

    # Collision-aware short labels at floor centroids.
    obstacles = []
    for c in d["creatures"]:
        obstacles.append(pt(c["XPosition"], c["YPosition"]))
    for c in d["placeables"]:
        obstacles.append(pt(c["X"], c["Y"]))
    for c in d["waypoints"]:
        obstacles.append(pt(c["XPosition"], c["YPosition"]))
    for c in d["doors"]:
        obstacles.append(pt(c["X"], c["Y"]))
    for h in d["hooks"]:
        obstacles.append(pt(h["x"], h["y"]))
    ex, ey = pt(d["entry"][0], d["entry"][1])
    obstacles.append((ex, ey))
    obstacles.append(pt(d["entry"][0], d["entry"][1] + 5.2))
    for t in (1.0, 2.0, 3.0, 4.0):
        obstacles.append(pt(d["entry"][0], d["entry"][1] + t))
    entry_text = ("IFO entry (%.2f, %.2f) dir +Y"
                  % (d["entry"][0], d["entry"][1]))
    ew = len(entry_text) * 5.9 + 8
    eb = [ex + 9, ey - 55, ex + 9 + ew, ey - 42]
    obstacle_boxes = [eb]
    placed_boxes = []
    labels = []
    offsets = [(0, -9), (0, 13), (-30, -9), (30, -9), (-30, 13), (30, 13),
               (0, -25), (0, 27), (-48, 0), (48, 0), (-48, -20), (48, 20)]
    for room in rooms:
        if not room["floor_faces"]:
            continue
        cx, cy = pt(*room["centroid"])
        lw = len(room["label"]) * 7.0 + 9
        lh = 14.0
        best = None
        for dx, dy in offsets:
            box = [cx + dx - lw / 2, cy + dy - lh / 2,
                   cx + dx + lw / 2, cy + dy + lh / 2]
            hits = sum(1 for x, y in obstacles
                       if box[0] - 3 <= x <= box[2] + 3
                       and box[1] - 3 <= y <= box[3] + 3)
            hits += 10 * sum(1 for b in obstacle_boxes
                             if not (box[2] < b[0] or box[0] > b[2]
                                     or box[3] < b[1] or box[1] > b[3]))
            hits += 10 * sum(1 for b in placed_boxes
                             if not (box[2] < b[0] or box[0] > b[2]
                                     or box[3] < b[1] or box[1] > b[3]))
            if best is None or hits < best[0]:
                best = (hits, box, cx + dx, cy + dy)
            if hits == 0:
                break
        _, box, tx, ty = best
        placed_boxes.append(box)
        labels.append((tx, ty, box, room["label"]))
    for tx, ty, box, label in labels:
        out.append('<rect class="rlabel-bg" x="%s" y="%s" width="%s" height="%s" '
                   'rx="2"/>'
                   % (fmt(box[0]), fmt(box[1]), fmt(box[2] - box[0]),
                      fmt(box[3] - box[1])))
        out.append(text(tx, ty + 3.8, label, "rlabel", "middle"))

    # Door hooks (diamond + quaternion yaw tick), then GIT door rings.
    out.append('<g id="hooks">')
    for h in d["hooks"]:
        X, Y = pt(h["x"], h["y"])
        yaw = math.radians(probe.qyaw(h["quat"]))
        X2 = X + math.cos(yaw) * 1.35 * scale
        Y2 = Y - math.sin(yaw) * 1.35 * scale
        out.append('<line class="hooktick" x1="%s" y1="%s" x2="%s" y2="%s"/>'
                   % (fmt(X), fmt(Y), fmt(X2), fmt(Y2)))
        out.append('<path class="hook" d="M %s %s l 4.2 0 l 0 4.2 l -4.2 0 z" '
                   'transform="rotate(45 %s %s)"/>'
                   % (fmt(X - 2.1), fmt(Y - 2.1), fmt(X), fmt(Y)))
    out.append("</g>")
    out.append('<g id="doors">')
    for door in d["door_items"]:
        X, Y = pt(door["position"][0], door["position"][1])
        out.append('<circle class="door" cx="%s" cy="%s" r="6.2"/>'
                   % (fmt(X), fmt(Y)))
    out.append("</g>")

    # Entities.
    out.append('<g id="placeables">')
    for c in d["placeables"]:
        X, Y = pt(c["X"], c["Y"])
        out.append('<rect class="placeable" x="%s" y="%s" width="5" height="5"/>'
                   % (fmt(X - 2.5), fmt(Y - 2.5)))
    out.append("</g>")
    out.append('<g id="waypoints">')
    for c in d["waypoints"]:
        X, Y = pt(c["XPosition"], c["YPosition"])
        out.append('<path class="waypoint" d="M %s %s l 3.6 6.2 l -7.2 0 z"/>'
                   % (fmt(X), fmt(Y - 3.1)))
    out.append("</g>")
    out.append('<g id="creatures">')
    for c in d["creatures"]:
        X, Y = pt(c["XPosition"], c["YPosition"])
        out.append('<circle class="creature" cx="%s" cy="%s" r="3.4"/>'
                   % (fmt(X), fmt(Y)))
    out.append("</g>")

    # Outlier rings: floor-height outliers and the one door offset.
    out.append('<g id="outliers">')
    wp_by_name = {c["Tag"]: c for c in d["waypoints"]}
    for tag in ("wp_room8_02",):
        c = wp_by_name.get(tag)
        if c:
            X, Y = pt(c["XPosition"], c["YPosition"])
            out.append('<circle class="ring-floor" cx="%s" cy="%s" r="9"/>'
                       % (fmt(X), fmt(Y)))
    for c in d["placeables"]:
        if c["TemplateResRef"] in ("plc_pwrcond", "end_explosion"):
            X, Y = pt(c["X"], c["Y"])
            out.append('<circle class="ring-floor" cx="%s" cy="%s" r="8"/>'
                       % (fmt(X), fmt(Y)))
    for door in d["door_items"]:
        if door["index"] == values["door_max_item_index"]:
            X, Y = pt(door["position"][0], door["position"][1])
            out.append('<circle class="ring-door" cx="%s" cy="%s" r="10"/>'
                       % (fmt(X), fmt(Y)))
    out.append("</g>")

    # IFO entry star and direction arrow.
    ex, ey = pt(d["entry"][0], d["entry"][1])
    tx, ty = pt(d["entry"][0], d["entry"][1] + 5.2)
    star = []
    for k in range(16):
        ang = -math.pi / 2 + k * math.pi / 8
        rad = 7.0 if k % 2 == 0 else 3.2
        star.append("%s,%s" % (fmt(ex + math.cos(ang) * rad),
                               fmt(ey + math.sin(ang) * rad)))
    out.append('<polygon class="entrystar" points="%s"/>' % " ".join(star))
    out.append('<line class="entryarrow" x1="%s" y1="%s" x2="%s" y2="%s"/>'
               % (fmt(ex), fmt(ey - 6), fmt(tx), fmt(ty + 7)))
    out.append('<path d="M %s %s L %s %s L %s %s z" fill="#e53935"/>'
               % (fmt(tx - 3.5), fmt(ty + 7), fmt(tx + 3.5), fmt(ty + 7),
                  fmt(tx), fmt(ty)))
    out.append(text(ex + 9, ey - 46, entry_text, "entrylabel"))

    # Off-map 07b indicator.
    off_x = sx(41.5069)
    out.append('<path d="M %s 50 L %s 38 M %s 50 l -3 4 M %s 50 l 3 4" '
               'stroke="#a66a00" stroke-width="1.2" fill="none"/>'
               % (fmt(off_x), fmt(off_x), fmt(off_x), fmt(off_x)))
    out.append(text(off_x, 64, "07b off-map (y=203.7, no WOK floor)",
                    "offmap", "middle"))

    # Scale bar.
    bar_y = map_b + 30
    bar_w = 10.0 * scale
    out.append('<g id="scalebar">')
    out.append('<line x1="%s" y1="%s" x2="%s" y2="%s" stroke="#33414f" '
               'stroke-width="2"/>'
               % (fmt(MAP_L), fmt(bar_y), fmt(MAP_L + bar_w), fmt(bar_y)))
    for frac in (0.0, 0.5, 1.0):
        X = MAP_L + bar_w * frac
        out.append('<line x1="%s" y1="%s" x2="%s" y2="%s" stroke="#33414f" '
                   'stroke-width="1.2"/>'
                   % (fmt(X), fmt(bar_y - 4), fmt(X), fmt(bar_y + 4)))
    out.append(text(MAP_L, bar_y + 18, "10 m", "tick"))
    out.append(text(MAP_L + bar_w + 10, bar_y + 18,
                    "scale %.2f px/m" % scale, "tick"))
    out.append("</g>")

    # ---------------- Side panel ----------------
    out.append('<rect class="panel" x="%s" y="24" width="%s" height="%s" '
               'rx="6"/>'
               % (fmt(panel_x), fmt(panel_w), fmt(CANVAS_H - 48)))
    x0 = panel_x + 22
    out.append(text(x0, 54, "m01aa geometry preview", "h1"))
    out.append(text(x0, 76, "Top-down world XY, +Y up; %0.2f px per metre."
                    % scale, "sub"))
    out.append(text(x0, 93, "Parsed from IFO/GIT/LYT/WOK/MDL/PTH; no engine or "
                    "game launch.", "sub"))
    out.append('<line class="rule" x1="%s" y1="104" x2="%s" y2="104"/>'
               % (fmt(x0), fmt(panel_x + panel_w - 20)))

    def sec(y, title):
        out.append('<line class="rule" x1="%s" y1="%s" x2="%s" y2="%s"/>'
                   % (fmt(x0), fmt(y - 15), fmt(panel_x + panel_w - 20),
                      fmt(y - 15)))
        out.append(text(x0, y, title, "sec"))

    def item(y, s, cls="item"):
        out.append(text(x0, y, s, cls))

    # Legend.
    sec(130, "LEGEND")
    ly = 152
    rows = []
    rows.append(("floor", "WOK floor: upward triangles (n.z > 0.5)"))
    rows.append(("outline", "WOK floor boundary (edges used once)"))
    rows.append(("rlabel", "LYT room label; + = LYT origin"))
    rows.append(("hook", "LYT door hook + quaternion yaw tick"))
    rows.append(("door", "GIT door position (ring)"))
    rows.append(("creature", "GIT creature"))
    rows.append(("placeable", "GIT placeable"))
    rows.append(("waypoint", "GIT waypoint"))
    rows.append(("pth", "PTH node and undirected link"))
    rows.append(("entry", "IFO entry point and +Y direction"))
    rows.append(("entryfloor", "Entry floor triangle (verified hit)"))
    rows.append(("ring-floor", "Floor-height outlier (> 0.10 m)"))
    rows.append(("ring-door", "Door/hook offset outlier (> 10 mm)"))
    for kind, label in rows:
        if kind == "floor":
            out.append('<rect class="swatch" x="%s" y="%s" width="14" height="10" '
                       'fill="#5b8ff9" fill-opacity="0.35" stroke="#33414f"/>'
                       % (fmt(x0), fmt(ly - 8)))
        elif kind == "outline":
            out.append('<line x1="%s" y1="%s" x2="%s" y2="%s" stroke="#33414f" '
                       'stroke-width="1.6"/>'
                       % (fmt(x0), fmt(ly - 3), fmt(x0 + 14), fmt(ly - 3)))
        elif kind == "rlabel":
            out.append(text(x0 + 1, ly - 1, "01a", "rlabel"))
        elif kind == "hook":
            out.append('<path class="hook" d="M %s %s l 4 0 l 0 4 l -4 0 z" '
                       'transform="rotate(45 %s %s)"/>'
                       % (fmt(x0 + 5), fmt(ly - 8), fmt(x0 + 7), fmt(ly - 5)))
            out.append('<line class="hooktick" x1="%s" y1="%s" x2="%s" y2="%s"/>'
                       % (fmt(x0 + 7), fmt(ly - 5), fmt(x0 + 15), fmt(ly - 5)))
        elif kind == "door":
            out.append('<circle class="door" cx="%s" cy="%s" r="6"/>'
                       % (fmt(x0 + 7), fmt(ly - 5)))
        elif kind == "creature":
            out.append('<circle class="creature" cx="%s" cy="%s" r="4"/>'
                       % (fmt(x0 + 7), fmt(ly - 5)))
        elif kind == "placeable":
            out.append('<rect class="placeable" x="%s" y="%s" width="8" height="8"/>'
                       % (fmt(x0 + 3), fmt(ly - 9)))
        elif kind == "waypoint":
            out.append('<path class="waypoint" d="M %s %s l 5 9 l -10 0 z"/>'
                       % (fmt(x0 + 7), fmt(ly - 12)))
        elif kind == "pth":
            out.append('<line class="plink" x1="%s" y1="%s" x2="%s" y2="%s"/>'
                       % (fmt(x0), fmt(ly - 4), fmt(x0 + 14), fmt(ly - 4)))
            out.append('<circle class="pnode" cx="%s" cy="%s" r="2.2"/>'
                       % (fmt(x0 + 7), fmt(ly - 4)))
        elif kind == "entry":
            out.append('<polygon class="entrystar" points="%s"/>'
                       % " ".join("%s,%s" % (fmt(x0 + 7 + math.cos(-math.pi / 2
                                                                  + k * math.pi / 4)
                                                 * (5 if k % 2 == 0 else 2.5)),
                                              fmt(ly - 5 + math.sin(-math.pi / 2
                                                                    + k * math.pi / 4)
                                                 * (5 if k % 2 == 0 else 2.5)))
                                  for k in range(8)))
        elif kind == "entryfloor":
            out.append('<polygon class="entryfloor" points="%s,%s %s,%s %s,%s"/>'
                       % (fmt(x0 + 1), fmt(ly - 1), fmt(x0 + 13), fmt(ly - 1),
                          fmt(x0 + 7), fmt(ly - 11)))
        elif kind == "ring-floor":
            out.append('<circle class="ring-floor" cx="%s" cy="%s" r="6"/>'
                       % (fmt(x0 + 7), fmt(ly - 5)))
        elif kind == "ring-door":
            out.append('<circle class="ring-door" cx="%s" cy="%s" r="6"/>'
                       % (fmt(x0 + 7), fmt(ly - 5)))
        out.append(text(x0 + 26, ly, label, "item"))
        ly += 19

    # Counts.
    sec(414, "COUNTS")
    counts = stats["counts"]
    c_lines = [
        "%d LYT rooms (%d with WOK floor) | %d door hooks"
        % (counts["lyt_rooms"], counts["rooms_with_wok_floor"],
           counts["lyt_door_hooks"]),
        "%d creatures | %d doors | %d placeables | %d waypoints"
        % (counts["git"]["creatures"], counts["git"]["doors"],
           counts["git"]["placeables"], counts["git"]["waypoints"]),
        "%d WOK vertices | %d WOK triangles | %d drawn floor triangles"
        % (counts["wok_vertices"], counts["wok_triangles"],
           counts["wok_floor_triangles"]),
        "%d PTH points | %d directed = %d undirected links | %d component(s)"
        % (counts["pth"]["points"], counts["pth"]["connections"],
           counts["pth"]["undirected_edges"], counts["pth"]["components"]),
    ]
    cy = 436
    for s in c_lines:
        item(cy, s, "mono")
        cy += 18

    # Verified checks.
    sec(536, "VERIFIED CHECKS")
    checks = [
        "GFF parse: IFO OK, GIT OK, PTH OK",
        "Entry floor: %s #%d, dz %0.4f m"
        % (stats["entry"]["floor_room"], stats["entry"]["floor_face"],
           stats["entry"]["dz_m"]),
        "Entry dir (%0.3f, %0.3f), norm error %0.1e"
        % (stats["entry"]["direction_xy"][0],
           stats["entry"]["direction_xy"][1],
           stats["entry"]["direction_norm_error"]),
        "WOK winding: +%d / -%d / 0 %d; min dot %0.4f"
        % (stats["wok"]["winding"]["positive"],
           stats["wok"]["winding"]["negative"],
           stats["wok"]["winding"]["zero"],
           stats["wok"]["winding"]["min_dot"]),
        "Door -> hook: max %0.3f mm; %d/%d hooks used"
        % (stats["doors"]["max_3d_mm"], stats["doors"]["hooks_used"],
           stats["counts"]["lyt_door_hooks"]),
        "Hook -> MDL node: max %0.3f mm; %d/%d found"
        % (stats["hook_to_mdl"]["max_err_mm"],
           stats["hook_to_mdl"]["found"], stats["hook_to_mdl"]["hook_count"]),
        "Shared WOK vertices: %d positions, %d room pairs"
        % (stats["wok"]["shared_vertices"]["positions"],
           len(stats["wok"]["shared_vertices"]["room_pairs"])),
        "PTH: %d directed = %d undirected; %d non-reciprocal; %d component(s)"
        % (stats["pth"]["connections"], stats["pth"]["undirected_edges"],
           stats["pth"]["non_reciprocal"], stats["pth"]["components"]["count"]),
        "Creature floor: max |dz| %0.3f mm (%d/%d hits)"
        % (stats["floor_conformance"]["creatures"]["max_abs_m"] * 1000.0,
           stats["floor_conformance"]["creatures"]["hits"],
           stats["floor_conformance"]["creatures"]["n"]),
        "Waypoint floor: %d/%d <= 1 cm; max %0.3f m"
        % (stats["floor_conformance"]["waypoints"]["within_threshold"],
           stats["floor_conformance"]["waypoints"]["n"],
           stats["floor_conformance"]["waypoints"]["max_abs_m"]),
        "Placeable floor: %d/%d <= 5 cm; max %0.3f m"
        % (stats["floor_conformance"]["placeables"]["within_threshold"],
           stats["floor_conformance"]["placeables"]["n"],
           stats["floor_conformance"]["placeables"]["max_abs_m"]),
        "MDL XY bbox covers WOK: %d yes / %d no / %d n/a"
        % (values["mdl_true"], values["mdl_false"], values["mdl_na"]),
    ]
    yy = 558
    for s in checks:
        item(yy, s, "mono")
        yy += 17

    # Discrepancies.
    sec(784, "DISCREPANCIES / NOT DRAWN")
    note_lines = []
    note_lines.extend(wrap(
        "end_door06 -> M01aa_09/Door_07: %.3f mm from its hook; all other "
        "doors <= %.3f mm." % (stats["discrepancies"][0]["value_mm"],
                               stats["discrepancies"][0]["other_doors_max_mm"]),
        76))
    note_lines.extend(wrap(
        "wp_room8_02: +%.3f m above floor; wp_cutblast02 +%.3f m; %d waypoints "
        "exceed 1 cm." % (
            stats["discrepancies"][1]["value_m"],
            abs(wp_worst[1]["delta_m"]) if len(wp_worst) > 1 else 0.0,
            stats["discrepancies"][1]["over_1cm"]), 76))
    note_lines.extend(wrap(
        "plc_pwrcond and end_explosion: +%.3f m above floor; %d placeables "
        "exceed 5 cm." % (stats["discrepancies"][2]["value_m"],
                          stats["discrepancies"][2]["over_5cm"]), 76))
    note_lines.extend(wrap(
        "M01aa_07b: no WOK floor (0 vertices); LYT origin off-map at y=203.7; "
        "MDL bbox %.0f x %.0f x %.0f m, so it is not drawn."
        % tuple(dims7) if dims7 else "M01aa_07b: no WOK floor; not drawn.", 76))
    note_lines.extend(wrap(
        "M01aa_08d: 2 floor triangles but no type-33 MDL trimesh; MDL bbox "
        "check n/a.", 76))
    note_lines.extend(wrap(
        "MDL bbox XY misses WOK in %d rooms (%s); max deficit %.1f mm."
        % (values["mdl_false"], ", ".join(x["room"] for x in false_mdl),
           values["max_deficit"]), 76))
    ny = 806
    for s in note_lines:
        item(ny, s, "warn")
        ny += 15

    # Room table.
    sec(976, "ROOMS (LYT ORDER)")
    item(996, "room        WOKv  WOKf  flr  MDLt", "mono")
    yy = 1014
    for room in rooms:
        mdl_txt = str(room["mdl_tri_nodes"])
        if room["key"] == "m01aa_07b":
            mdl_txt = "sky"
        item(yy, "%-11s %4d  %4d  %3d  %4s"
             % (room["name"], len(room["wok"].verts),
                len(room["wok"].faces), len(room["floor_faces"]), mdl_txt),
             "mono")
        yy += 16
    item(yy + 2, "WOKv/WOKf = vertices/triangles; flr = drawn floor; "
         "MDLt = type-33 nodes; sky = no WOK floor", "sub")

    # Bounds.
    sec(1298, "BOUNDS (m)")
    b = stats["bounds_m"]
    b_lines = [
        "WOK XYZ   %s .. %s" % (
            " ".join("%.2f" % v for v in b["wok_xyz"][0]),
            " ".join("%.2f" % v for v in b["wok_xyz"][1])),
        "Entities  %s .. %s" % (
            " ".join("%.2f" % v for v in b["entities_xyz"][0]),
            " ".join("%.2f" % v for v in b["entities_xyz"][1])),
        "PTH XY    %.2f,%.2f .. %.2f,%.2f; floor z %.3f .. %.3f" % (
            b["pth_floor_xyz"][0][0], b["pth_floor_xyz"][0][1],
            b["pth_floor_xyz"][1][0], b["pth_floor_xyz"][1][1],
            stats["pth"]["floor_z"]["min"], stats["pth"]["floor_z"]["max"]),
        "Render    %.2f,%.2f .. %.2f,%.2f" % (
            b["render_xy"][0], b["render_xy"][1],
            b["render_xy"][2], b["render_xy"][3]),
        "Entry     %.3f, %.3f, %.3f; direction (%.1f, %.1f)" % (
            stats["entry"]["position"][0], stats["entry"]["position"][1],
            stats["entry"]["position"][2], stats["entry"]["direction_xy"][0],
            stats["entry"]["direction_xy"][1]),
    ]
    yy = 1320
    for s in b_lines:
        item(yy, s, "mono")
        yy += 17

    # Method note.
    sec(1440, "METHOD")
    method = ("Floor fills use WOK triangles with normal.z > 0.5 and projected "
              "area >= 1e-6 m2; outlines are WOK floor edges used by exactly "
              "one drawn triangle (LYT supplies room origins only). PTH links "
              "are de-duplicated undirected pairs. Z is not encoded in this "
              "top-down view except through the floor fill. Values were "
              "cross-checked against probe.py stdout.")
    yy = 1462
    for s in wrap(method, 78):
        item(yy, s, "item")
        yy += 16

    out.append(text(x0, CANVAS_H - 28,
                    "Source: artifacts/analysis/coordinates2/extract "
                    "(gitignored). Lane 09 generated artifact.",
                    "sub"))
    out.append("</svg>")
    return "\n".join(out) + "\n", scale, dims7, wp_worst, false_mdl


def build_summary(stats, scale, dims7, wp_worst, false_mdl):
    s = stats
    text = f"""# m01aa geometry preview

`preview.svg` is a dependency-free top-down render (world XY, +Y up, {scale:.2f} px/m): {stats['rendering']['floor_triangles_drawn']} of {s['counts']['wok_triangles']} WOK triangles are drawn as upward-facing floor, with outlines across {stats['counts']['rooms_with_wok_floor']} rooms, LYT room labels, {s['counts']['lyt_door_hooks']} door hooks, {s['counts']['git']['doors']} GIT doors, {s['counts']['git']['creatures']} creatures, {s['counts']['git']['placeables']} placeables, {s['counts']['git']['waypoints']} waypoints, {s['counts']['pth']['points']} PTH nodes/{s['counts']['pth']['undirected_edges']} links, and the IFO entry point/direction.

LYT supplies room origins, not outlines; outlines are WOK floor boundary edges, and labels sit at floor centroids with each LYT origin marked by a cross.

Checks match `probe.py`: IFO/GIT/PTH parse clean. Entry ({s['entry']['position'][0]:.3f}, {s['entry']['position'][1]:.3f}, {s['entry']['position'][2]:.3f}) hits M01aa_01a face 55 with dz {s['entry']['dz_m']:.4f}; direction is unit +Y. WOK winding is {s['wok']['winding']['positive']:+d}/{s['wok']['winding']['negative']:+d}/0, minimum normal dot {s['wok']['winding']['min_dot']:.4f}. All 15 doors map to hooks (max 3D offset {s['doors']['max_3d_mm']:.3f} mm); all 15 hooks match MDL nodes (max {s['hook_to_mdl']['max_err_mm']:.3f} mm). {s['wok']['shared_vertices']['positions']} shared WOK vertex positions. PTH: {s['pth']['connections']} directed = {s['pth']['undirected_edges']} undirected, {s['pth']['non_reciprocal']} non-reciprocal, {s['pth']['components']['count']} component.

Discrepancies: end_door06 at M01aa_09/Door_07 is {s['discrepancies'][0]['value_mm']:.3f} mm from its hook (others <= {s['discrepancies'][0]['other_doors_max_mm']:.3f} mm). wp_room8_02 is {s['discrepancies'][1]['value_m']:.3f} m above floor; plc_pwrcond and end_explosion are {s['discrepancies'][2]['value_m']:.3f} m above. MDL XY bbox misses WOK in {len(false_mdl)} rooms (max {s['mdl_bbox_contains_wok_xy']['max_false_deficit_mm']:.1f} mm). M01aa_07b has no WOK floor and is not drawn; M01aa_08d has no MDL trimesh.
"""
    return text


def validate_svg(path):
    root = ET.parse(path).getroot()
    if not root.tag.endswith("svg"):
        raise RuntimeError("root is not svg: %s" % root.tag)
    ns = "{http://www.w3.org/2000/svg}"
    counts = {
        "path": len(root.findall(".//" + ns + "path")),
        "polygon": len(root.findall(".//" + ns + "polygon")),
        "circle": len(root.findall(".//" + ns + "circle")),
        "rect": len(root.findall(".//" + ns + "rect")),
        "line": len(root.findall(".//" + ns + "line")),
        "text": len(root.findall(".//" + ns + "text")),
    }
    size = os.path.getsize(path)
    if size < 20000:
        raise RuntimeError("SVG too small: %d bytes" % size)
    if counts["text"] < 30 or counts["path"] < 20:
        raise RuntimeError("SVG lacks expected content: %r" % counts)
    raw = open(path, "rb").read().decode("utf-8")
    if "nan" in raw.lower() or "None" in raw:
        raise RuntimeError("SVG contains nan/None markers")
    return size, counts


def main():
    d = gather()
    stats, values = build_stats(d)
    values["door_max_item_index"] = (min(d["door_items"],
                                         key=lambda x: -x["dist_m"])["index"])
    stats["probe_parity"] = verify_probe_parity(stats, values)

    svg, scale, dims7, wp_worst, false_mdl = build_svg(d, stats, values)
    with open(SVG_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write(svg)
    stats["rendering"]["svg_bytes"] = os.path.getsize(SVG_PATH)
    stats["rendering"]["scale_px_per_m"] = r(scale, 4)

    with open(STATS_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump(stats, f, indent=2, sort_keys=False)
        f.write("\n")

    summary = build_summary(stats, scale, dims7, wp_worst, false_mdl)
    words = len(summary.split())
    if words >= 200:
        raise RuntimeError("summary.md has %d words (limit 200)" % words)
    with open(SUMMARY_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write(summary)

    size, counts = validate_svg(SVG_PATH)
    print("wrote", SVG_PATH)
    print("  svg bytes", size, "elements", counts)
    print("  scale %.4f px/m, world window %s" % (scale, stats["bounds_m"]["render_xy"]))
    print("wrote", STATS_PATH, "(%d bytes)" % os.path.getsize(STATS_PATH))
    print("wrote", SUMMARY_PATH, "(%d words)" % words)
    print("probe parity:", stats["probe_parity"]["ok"],
          "(%d checks)" % stats["probe_parity"]["checks"])
    print("counts:", stats["counts"])


if __name__ == "__main__":
    main()
