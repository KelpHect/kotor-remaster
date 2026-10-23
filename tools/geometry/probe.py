#!/usr/bin/env python3
"""Read-only m01aa geometry probe (LYT/WOK/MDL/GFF/PTH check values).

Set KOTOR_AREA_DIR to a flat directory containing m01aa.ifo/.git/.pth/.lyt,
<room>.wok and <room>.mdl files extracted from the user's install.
"""
from __future__ import annotations

import importlib.util
import math
import os
import struct
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BASE = os.environ.get("KOTOR_AREA_DIR",
                      os.path.join(ROOT, "artifacts", "analysis", "coordinates2", "extract"))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gff = load("lane09_gff", os.path.join(ROOT, "tools", "formats", "gff.py"))


class Wok:
    def __init__(self, path):
        with open(path, "rb") as f:
            b = f.read()
        if b[:8] != b"BWM V1.0":
            raise ValueError("not BWM V1.0: " + path)
        self.path, self.buf = path, b
        self.type = struct.unpack_from("<I", b, 8)[0]
        (self.vc, self.vo, self.fc, self.fo, self.mo, self.no, self.do,
         self.ac, self.ao, self.unk, self.adjc, self.adjoff,
         self.ec, self.eo, self.pc, self.po) = struct.unpack_from("<16I", b, 72)
        self.verts = [struct.unpack_from("<3f", b, self.vo + i * 12)
                      for i in range(self.vc)]
        self.faces = [struct.unpack_from("<3I", b, self.fo + i * 12)
                      for i in range(self.fc)]
        self.normals = ([struct.unpack_from("<3f", b, self.no + i * 12)
                         for i in range(self.fc)] if self.type == 1 else [])


class Mdl:
    def __init__(self, path):
        with open(path, "rb") as f:
            b = f.read()
        self.buf, self.path = b, path
        self.name = b[20:52].split(b"\0")[0].decode("latin-1")
        self.root_offset, self.node_count = struct.unpack_from("<2I", b, 52)
        self.supermodel = b[148:180].split(b"\0")[0].decode("latin-1")
        (self.unknown3, self.mdx_size, self.mdx_offset,
         self.offset_to_name_offsets, self.name_offsets_count,
         self.name_offsets_count2) = struct.unpack_from("<6I", b, 184)
        base = 12 + self.offset_to_name_offsets
        self.name_offsets = list(struct.unpack_from(
            "<%dI" % self.name_offsets_count, b, base)) if self.name_offsets_count else []
        self.nodes = {}
        if self.root_offset:
            self._walk(self.root_offset, 0)

    def _name(self, i):
        if not self.name_offsets:
            return ""
        o = 12 + self.name_offsets[i]
        return self.buf[o:].split(b"\0")[0].decode("latin-1")

    def _walk(self, off, depth):
        b, D = self.buf, 12
        a = D + off
        nt, ni, nni, pad = struct.unpack_from("<4H", b, a)
        parent = struct.unpack_from("<I", b, a + 12)[0]
        pos = struct.unpack_from("<3f", b, a + 16)
        quat = struct.unpack_from("<4f", b, a + 28)
        child_arr, child_n, child_n2 = struct.unpack_from("<3I", b, a + 44)
        (ctrl_arr, ctrl_n, ctrl_n2, ctrl_dat,
         ctrl_dn, ctrl_dn2) = struct.unpack_from("<6I", b, a + 56)
        kids = ([struct.unpack_from("<I", b, D + child_arr + i * 4)[0]
                 for i in range(child_n)] if child_n else [])
        node = {"off": off, "type": nt, "index": ni, "name": self._name(nni),
                "parent": parent, "pos": pos, "quat": quat,
                "kids": kids, "depth": depth}
        if nt == 33:
            t = a + 80
            node["bbox_min"] = struct.unpack_from("<3f", b, t + 20)
            node["bbox_max"] = struct.unpack_from("<3f", b, t + 32)
        self.nodes[off] = node
        for k in kids:
            if k not in self.nodes:
                self._walk(k, depth + 1)


def qmat(q):
    w, x, y, z = q
    n = w * w + x * x + y * y + z * z
    if n == 0:
        return ((1, 0, 0), (0, 1, 0), (0, 0, 1))
    s = 2.0 / n
    xx, yy, zz = x * x * s, y * y * s, z * z * s
    xy, xz, yz = x * y * s, x * z * s, y * z * s
    wx, wy, wz = w * x * s, w * y * s, w * z * s
    return ((1 - yy - zz, xy - wz, xz + wy),
            (xy + wz, 1 - xx - zz, yz - wx),
            (xz - wy, yz + wx, 1 - xx - yy))


def mm(a, b):
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3))
                 for i in range(3))


def mv(m, v):
    return tuple(sum(m[i][j] * v[j] for j in range(3)) for i in range(3))


def node_world(mdl, node):
    chain = []
    cur = node
    seen = set()
    while cur is not None and cur["off"] not in seen:
        seen.add(cur["off"])
        chain.append(cur)
        p = cur["parent"]
        cur = mdl.nodes.get(p) if p in mdl.nodes else None
    R = ((1, 0, 0), (0, 1, 0), (0, 0, 1))
    t = (0.0, 0.0, 0.0)
    for nd in reversed(chain):
        t = tuple(t[i] + sum(R[i][j] * nd["pos"][j] for j in range(3)) for i in range(3))
        R = mm(R, qmat(nd["quat"]))
    return R, t


def parse_lyt(path):
    rooms, hooks = [], []
    mode = None
    with open(path, "r", encoding="latin-1") as f:
        for ln in f:
            s = ln.strip()
            if not s or s.startswith("#"):
                continue
            low = s.lower()
            if low.startswith("roomcount"):
                mode = "room"
            elif low.startswith("doorhookcount"):
                mode = "door"
            elif low.startswith(("trackcount", "obstaclecount", "donelayout", "beginlayout")):
                mode = None
            elif mode == "room":
                p = s.split()
                rooms.append({"room": p[0], "x": float(p[1]), "y": float(p[2]),
                              "z": float(p[3])})
            elif mode == "door":
                p = s.split()
                hooks.append({"room": p[0], "node": p[1],
                              "x": float(p[3]), "y": float(p[4]), "z": float(p[5]),
                              "quat": tuple(float(v) for v in p[6:10])})
    return rooms, hooks


def pip(px, py, tri, eps=1e-7):
    (x1, y1), (x2, y2), (x3, y3) = tri
    d = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3)
    if abs(d) < 1e-12:
        return None
    a = ((y2 - y3) * (px - x3) + (x3 - x2) * (py - y3)) / d
    b = ((y3 - y1) * (px - x3) + (x1 - x3) * (py - y3)) / d
    c = 1.0 - a - b
    if a >= -eps and b >= -eps and c >= -eps:
        return (a, b, c)
    return None


def qyaw(q):
    w, x, y, z = q
    return math.degrees(2.0 * math.atan2(z, w))


def norm_angle(a):
    while a <= -180:
        a += 360
    while a > 180:
        a -= 360
    return a


def main():
    ifo = gff.Gff(os.path.join(BASE, "m01aa.ifo"))
    git = gff.Gff(os.path.join(BASE, "m01aa.git"))
    pth = gff.Gff(os.path.join(BASE, "m01aa.pth"))
    print("GFF check IFO", gff.check(ifo))
    print("GFF check GIT", gff.check(git))
    print("GFF check PTH", gff.check(pth))
    d = ifo.as_dict(0)
    entry = (d["Mod_Entry_X"], d["Mod_Entry_Y"], d["Mod_Entry_Z"])
    edir = (d["Mod_Entry_Dir_X"], d["Mod_Entry_Dir_Y"])
    print("entry", entry, "dir", edir, "|dir|-1", abs(math.hypot(*edir) - 1))
    print("area list", [ifo.as_dict(si) for f in ifo.fields_of(0)
                        if f.label == "Mod_Area_list" for si in ifo.list_of(f)[1]])

    rooms, hooks = parse_lyt(os.path.join(BASE, "m01aa.lyt"))
    print("rooms", len(rooms), "hooks", len(hooks))
    woks, mdls, extents = {}, {}, {}
    for r in rooms:
        name = r["room"].lower()
        w = Wok(os.path.join(BASE, name + ".wok"))
        woks[name] = w
        mdls[name] = Mdl(os.path.join(BASE, name + ".mdl"))
        pts = []
        for nd in mdls[name].nodes.values():
            if nd["type"] != 33:
                continue
            R, t = node_world(mdls[name], nd)
            for i in (0, 1):
                for j in (0, 1):
                    for k in (0, 1):
                        c = (nd["bbox_max"][0] if i else nd["bbox_min"][0],
                             nd["bbox_max"][1] if j else nd["bbox_min"][1],
                             nd["bbox_max"][2] if k else nd["bbox_min"][2])
                        pts.append(tuple(t[q] + sum(R[q][s] * c[s] for s in range(3))
                                         for q in range(3)))
        if pts:
            extents[name] = (r["x"] + min(p[0] for p in pts),
                             r["y"] + min(p[1] for p in pts),
                             r["z"] + min(p[2] for p in pts),
                             r["x"] + max(p[0] for p in pts),
                             r["y"] + max(p[1] for p in pts),
                             r["z"] + max(p[2] for p in pts))
    print("MDL extents:")
    for r in rooms:
        name = r["room"].lower()
        w = woks[name]
        md = extents.get(name)
        if w.vc:
            wb = (min(v[0] for v in w.verts), min(v[1] for v in w.verts),
                  min(v[2] for v in w.verts), max(v[0] for v in w.verts),
                  max(v[1] for v in w.verts), max(v[2] for v in w.verts))
        else:
            wb = None
        print(f"  {r['room']:10s} origin=({r['x']:.3f},{r['y']:.3f}) wok={wb} mdl={None if not md else tuple(round(x,2) for x in md)}")
        if wb:
            mc = (md is not None and name != "m01aa_07b"
                  and md[0] <= wb[0] and md[1] <= wb[1] and md[3] >= wb[3] and md[4] >= wb[4])
            print(f"              origin_in_wok={wb[0] <= r['x'] <= wb[3] and wb[1] <= r['y'] <= wb[4]}"
                  f" mdl_contains_wok_xy={mc}")

    # shared vertices
    bypos = defaultdict(set)
    for r in rooms:
        w = woks[r["room"].lower()]
        for v in w.verts:
            bypos[(round(v[0], 4), round(v[1], 4), round(v[2], 4))].add(r["room"].lower())
    shared = {p: rr for p, rr in bypos.items() if len(rr) > 1}
    print("shared vertex positions", len(shared))
    pairs = defaultdict(int)
    for p, rr in shared.items():
        rr = sorted(rr)
        for i in range(len(rr)):
            for j in range(i + 1, len(rr)):
                pairs[(rr[i], rr[j])] += 1
    print("shared pairs", sorted(pairs.items(), key=lambda kv: -kv[1])[:20])

    # winding
    pos = neg = zero = 0
    min_dot = 1e9
    nzmin, nzmax = 9, -9
    for r in rooms:
        w = woks[r["room"].lower()]
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
            nzmin, nzmax = min(nzmin, n[2]), max(nzmax, n[2])
    print(f"winding +{pos} -{neg} 0{zero} min_dot={min_dot:.6g} nz=[{nzmin:.4f},{nzmax:.4f}]")

    # floor candidates
    def floors(px, py):
        out = []
        for r in rooms:
            w = woks[r["room"].lower()]
            for fi, f in enumerate(w.faces):
                tri = [(w.verts[i][0], w.verts[i][1]) for i in f]
                bc = pip(px, py, tri)
                if bc is None:
                    continue
                z = sum(bc[k] * w.verts[f[k]][2] for k in range(3))
                out.append({"room": r["room"], "face": fi, "z": z, "bary": bc})
        return out

    print("entry floors:")
    for h in floors(entry[0], entry[1]):
        print("  ", h["room"], h["face"], round(h["z"], 6), "dz", round(entry[2] - h["z"], 6), "bary", tuple(round(x, 6) for x in h["bary"]))

    # doors
    def git_list(label):
        for f in git.fields_of(0):
            if f.type == 15 and f.label == label:
                n, idx = git.list_of(f)
                return [git.as_dict(si) for si in idx]
        return []
    creatures = git_list("Creature List")
    doors = git_list("Door List")
    waypoints = git_list("WaypointList")
    placeables = git_list("Placeable List")
    print("counts", len(creatures), len(doors), len(waypoints), len(placeables))

    print("door vs hook:")
    used = set()
    max_xy = max_z = max_3d = 0
    for i, dd in enumerate(doors):
        h = min(hooks, key=lambda h: math.hypot(h["x"] - dd["X"], h["y"] - dd["Y"]))
        used.add((h["room"], h["node"]))
        xy = math.hypot(h["x"] - dd["X"], h["y"] - dd["Y"])
        dz = abs(h["z"] - dd["Z"])
        d3 = math.dist((h["x"], h["y"], h["z"]), (dd["X"], dd["Y"], dd["Z"]))
        max_xy, max_z, max_3d = max(max_xy, xy), max(max_z, dz), max(max_3d, d3)
        plane = abs(((math.degrees(dd["Bearing"]) - qyaw(h["quat"]) + 90) % 180) - 90)
        print(f"  {i:2d} {dd['Tag']:<16s} hook={h['room']}/{h['node']:<8s} xy={xy*1000:8.3f}mm z={dz*1000:8.3f}mm 3d={d3*1000:8.3f}mm plane={plane:6.3f}deg")
    print("  max_xy", max_xy, "max_z", max_z, "max_3d", max_3d, "unique hooks", len(used))
    print("door hooks not used:", [h for h in hooks if (h["room"], h["node"]) not in used])

    # mdl hooks
    print("LYT hook vs MDL node:")
    maxmdl = 0
    for h in hooks:
        name = h["room"].lower()
        mdl = mdls[name]
        nd = next((n for n in mdl.nodes.values() if n["name"].lower() == h["node"].lower()), None)
        if nd is None:
            print("  MISSING", h["room"], h["node"])
            continue
        R, t = node_world(mdl, nd)
        r = next(r for r in rooms if r["room"].lower() == name)
        w = (r["x"] + t[0], r["y"] + t[1], r["z"] + t[2])
        err = math.dist(w, (h["x"], h["y"], h["z"]))
        maxmdl = max(maxmdl, err)
        print(f"  {h['room']}/{h['node']:<8s} type={nd['type']:3d} err={err*1000:8.4f}mm depth={nd['depth']}")
    print("  max", maxmdl)

    # floor deltas
    def nearest_floor(px, py, refz):
        c = floors(px, py)
        if not c:
            return None, 0
        c.sort(key=lambda h: abs(h["z"] - refz))
        return c[0], len(c)
    print("creature floors:")
    deltas = []
    for c in creatures:
        h, n = nearest_floor(c["XPosition"], c["YPosition"], c["ZPosition"])
        if h is None:
            print("  MISS", c["TemplateResRef"], c["XPosition"], c["YPosition"])
            continue
        deltas.append(c["ZPosition"] - h["z"])
    print("  n", len(deltas), "max abs", max(abs(x) for x in deltas), "range", min(deltas), max(deltas))
    print("waypoint floors:")
    wd = []
    for c in waypoints:
        h, n = nearest_floor(c["XPosition"], c["YPosition"], c["ZPosition"])
        wd.append((c["ZPosition"] - h["z"] if h else None, n, c["Tag"]))
    print("  n", len(wd), "miss", sum(1 for x in wd if x[0] is None),
          "max abs", max(abs(x[0]) for x in wd if x[0] is not None),
          "within 1cm", sum(1 for x in wd if x[0] is not None and abs(x[0]) < 0.01))
    print("  worst", sorted([x for x in wd if x[0] is not None], key=lambda x: -abs(x[0]))[:8])
    print("placeable floors:")
    pd = []
    for c in placeables:
        h, n = nearest_floor(c["X"], c["Y"], c["Z"])
        pd.append((c["Z"] - h["z"] if h else None, n, c["TemplateResRef"]))
    print("  n", len(pd), "miss", sum(1 for x in pd if x[0] is None),
          "within 5cm", sum(1 for x in pd if x[0] is not None and abs(x[0]) < 0.05),
          "max", max(abs(x[0]) for x in pd if x[0] is not None))
    print("  elevated", sorted([x for x in pd if x[0] is not None], key=lambda x: -abs(x[0]))[:10])

    # PTH
    pts = []
    cons = []
    for f in pth.fields_of(0):
        if f.label == "Path_Points":
            n, idx = pth.list_of(f)
            pts = [(pth.as_dict(si)["X"], pth.as_dict(si)["Y"]) for si in idx]
        if f.label == "Path_Conections":
            n, idx = pth.list_of(f)
            cons = [pth.as_dict(si)["Destination"] for si in idx]
    print("pth pts", len(pts), "cons", len(cons), "dest range", min(cons), max(cons), "bad", sum(1 for x in cons if x < 0 or x >= len(pts)))
    root = pth.structs[0]
    # reconstruct edges from point connection ranges
    edges = []
    for si in [si for f in pth.fields_of(0) if f.label == "Path_Points" for si in pth.list_of(f)[1]]:
        d = pth.as_dict(si)
        k = d["Conections"]
        first = d["First_Conection"]
        # point index
        # find index by struct index
        idxs = [s for f in pth.fields_of(0) if f.label == "Path_Points" for s in pth.list_of(f)[1]]
        u = idxs.index(si)
        for q in range(k):
            edges.append((u, cons[first + q]))
    print("pth edges", len(edges), "first", edges[:10])
    # symmetry using undirected sets, duplicates
    es = set((min(u, v), max(u, v)) for u, v in edges)
    print("unique undirected", len(es), "dups", len(edges) - len(es))
    rev = sum(1 for u, v in edges if (v, u) not in set(edges))
    print("non-reciprocal", rev)
    adj = defaultdict(set)
    for u, v in edges:
        adj[u].add(v); adj[v].add(u)
    seen = set(); comps = []
    for i in range(len(pts)):
        if i in seen: continue
        stack = [i]; comp = []
        seen.add(i)
        while stack:
            u = stack.pop(); comp.append(u)
            for v in adj[u]:
                if v not in seen:
                    seen.add(v); stack.append(v)
        comps.append(comp)
    print("components", len(comps), sorted((len(c) for c in comps), reverse=True))
    zs = []
    multi = 0
    for (x, y) in pts:
        c = floors(x, y)
        if not c:
            zs.append(None); continue
        if len(c) > 1:
            multi += 1
        c.sort(key=lambda h: h["z"])
        zs.append(c[0]["z"])
    print("pth z range", min(z for z in zs if z is not None), max(z for z in zs if z is not None), "multi", multi,
          "missing", sum(1 for z in zs if z is None))
    # connection lengths in XY
    lens = [math.hypot(pts[u][0] - pts[v][0], pts[u][1] - pts[v][1]) for u, v in edges]
    print("edge len min/max/mean", min(lens), max(lens), sum(lens) / len(lens))

    # entity orientation checks
    ori = [abs(math.hypot(c["XOrientation"], c["YOrientation"]) - 1) for c in creatures]
    dev = []
    for c in creatures:
        a = math.degrees(math.atan2(c["YOrientation"], c["XOrientation"]))
        dev.append(abs(a - round(a / 11.25) * 11.25))
    print("creature |ori|-1 max", max(ori), "grid dev max", max(dev))
    bdev = []
    for c in placeables:
        a = math.degrees(c["Bearing"])
        bdev.append(abs(a - round(a / 11.25) * 11.25))
    print("placeable bearing grid dev max", max(bdev), "range", min(math.degrees(c["Bearing"]) for c in placeables), max(math.degrees(c["Bearing"]) for c in placeables))

    # entity bounds
    allpts = [entry] + [(c["XPosition"], c["YPosition"], c["ZPosition"]) for c in creatures] + \
             [(c["X"], c["Y"], c["Z"]) for c in doors] + \
             [(c["XPosition"], c["YPosition"], c["ZPosition"]) for c in waypoints] + \
             [(c["X"], c["Y"], c["Z"]) for c in placeables]
    print("entity bounds", tuple(round(min(p[i] for p in allpts), 3) for i in range(3)), tuple(round(max(p[i] for p in allpts), 3) for i in range(3)))
    print("pth bounds", min(p[0] for p in pts), max(p[0] for p in pts), min(p[1] for p in pts), max(p[1] for p in pts))


if __name__ == "__main__":
    main()
