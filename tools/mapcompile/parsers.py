"""Reusable read-only parsers for the map compiler (lane s1-03).

Logic mirrors tools/geometry/probe.py (m01aa-verified) plus the MDL mesh
header decode from docs/recon/15 (stride/vc/tex/MDX-offset fields) and the
BWM per-face surface array discovered at the face-array tail.  Nothing here
writes to the game install; GFF payloads are parsed through
tools/formats/gff.py via a scratch file because that parser takes a path.

Layouts (all little-endian):
  BWM/WOK V1.0: "BWM V1.0" + u32 type; 16 u32 at +72 =
    vc,vo, fc,fo, mo,no, do,ac,ao, unk, adjc,adjoff, ec,eo, pc,po.
    verts 3f at vo; faces 12 B at fo (3 u16 indices + 3 u16 aux, aux is 0 in
    all 1,365 sampled m01aa faces); per-face u32 surface id at mo (mo ==
    fo + fc*12 in every sampled file); normals 3f per face at no when
    type==1.  Vertices are world-space (recon/04 T07).
  LYT: "#MAXLAYOUT" text; roomcount/doorhookcount blocks (probe.py logic).
  VIS: text blocks "<room> <n>" followed by n room names.
  MDL: 12 B file header; model header at 12 (root +40, node count +44,
    supermodel +136, name table +184/+188); node header 80 B (type u16 +0,
    supernode +2, node number/name-index +4, parent u32 +12, pos 3f +16,
    quat WXYZ 4f +28, children +44/+48); mesh header at node+80
    (face off +8 rel 12, face count +12, stride +252,
    low16 vc / high16 texcount +304, MDX block offset +324).
  MDX: no header; per-mesh block at MDX offset: vc records of `stride`
    bytes (pos 3f, normal 3f, uv0 2f @24, uv1 2f @32 when texcount>=2, ...).
"""

from __future__ import annotations

import math
import os
import struct
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
for _p in (os.path.join(_ROOT, "tools", "formats"),
           os.path.join(_ROOT, "tools", "resources")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import gff as _gff  # tools/formats/gff.py


class ParseError(ValueError):
    pass


def gff_from_bytes(data: bytes, name: str = "<memory>"):
    """Parse GFF bytes through tools/formats/gff.py (path-based API)."""
    fd, path = tempfile.mkstemp(suffix=".gff", prefix="mapcompile_")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        g = _gff.Gff(path)
        g.path = name
        return g
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


# --------------------------------------------------------------------------
# BWM / WOK
# --------------------------------------------------------------------------

class Wok:
    """Parsed BWM V1.0 walkmesh.  verts/faces are world-space."""

    def __init__(self, data: bytes, name: str = "<wok>"):
        self.name = name
        self.buf = data
        if data[:8] != b"BWM V1.0":
            raise ParseError(f"{name}: not BWM V1.0")
        self.type = struct.unpack_from("<I", data, 8)[0]
        (self.vc, self.vo, self.fc, self.fo, self.mo, self.no, self.do,
         self.ac, self.ao, self.unk, self.adjc, self.adjoff,
         self.ec, self.eo, self.pc, self.po) = struct.unpack_from("<16I", data, 72)
        if self.vo + self.vc * 12 > len(data) or self.fo + self.fc * 12 > len(data):
            raise ParseError(f"{name}: vertex/face arrays exceed file")
        self.verts = [struct.unpack_from("<3f", data, self.vo + i * 12)
                      for i in range(self.vc)]
        self.faces = []
        self.face_aux = []
        for i in range(self.fc):
            u = struct.unpack_from("<6H", data, self.fo + i * 12)
            self.faces.append((u[0], u[2], u[4]))
            self.face_aux.append((u[1], u[3], u[5]))
        for f in self.faces:
            if max(f) >= self.vc:
                raise ParseError(f"{name}: face index out of range")
        # per-face u32 surface/material id array directly after the faces
        self.surface = []
        if self.mo and self.mo + self.fc * 4 <= len(data):
            self.surface = list(struct.unpack_from(f"<{self.fc}I", data,
                                                   self.mo)) if self.fc else []
        else:
            self.surface = [0] * self.fc
        self.normals = ([struct.unpack_from("<3f", data, self.no + i * 12)
                         for i in range(self.fc)]
                        if self.type == 1 and self.no else [])
        if self.type == 1 and len(self.normals) != self.fc:
            raise ParseError(f"{name}: type 1 without per-face normals")

    def bounds(self):
        if not self.verts:
            return None
        return (min(v[0] for v in self.verts), min(v[1] for v in self.verts),
                min(v[2] for v in self.verts), max(v[0] for v in self.verts),
                max(v[1] for v in self.verts), max(v[2] for v in self.verts))


def face_cross_dot(wok: Wok, fi: int):
    """cross(v1-v0, v2-v0) . normal for a face (winding oracle, recon/04 T08)."""
    a, b, c = (wok.verts[i] for i in wok.faces[fi])
    u = tuple(b[i] - a[i] for i in range(3))
    v = tuple(c[i] - a[i] for i in range(3))
    cr = (u[1] * v[2] - u[2] * v[1],
          u[2] * v[0] - u[0] * v[2],
          u[0] * v[1] - u[1] * v[0])
    if wok.normals:
        n = wok.normals[fi]
        return sum(cr[i] * n[i] for i in range(3)), math.sqrt(sum(x * x for x in cr))
    return None, math.sqrt(sum(x * x for x in cr))


# --------------------------------------------------------------------------
# LYT / VIS (text)
# --------------------------------------------------------------------------

def parse_lyt(text: str):
    """Return (rooms, hooks): rooms=[{room,x,y,z}], hooks=[{room,node,x,y,z,quat}]."""
    rooms, hooks = [], []
    mode = None
    for ln in text.splitlines():
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


def parse_vis(text: str):
    """Return {room_lower: [visible room names]} from the VIS text graph."""
    out = {}
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    i = 0
    while i < len(lines):
        parts = lines[i].split()
        if len(parts) == 2 and parts[1].isdigit():
            room = parts[0].lower()
            n = int(parts[1])
            out[room] = [lines[i + 1 + k].split()[0] for k in range(n)
                         if i + 1 + k < len(lines)]
            i += 1 + n
        else:
            i += 1
    return out


# --------------------------------------------------------------------------
# MDL / MDX
# --------------------------------------------------------------------------

MESH_TYPES = {0x21: "trimesh", 0x61: "skin", 0xA1: "animmesh",
              0x121: "dangly", 0x221: "walkmesh", 0x821: "saber"}


class Mdl:
    """MDL V1.0 node hierarchy + mesh headers (recon/15 layout)."""

    def __init__(self, data: bytes, name: str = "<mdl>"):
        self.name = name
        self.buf = data
        if len(data) < 200:
            raise ParseError(f"{name}: shorter than MDL model header")
        self.root_offset, self.node_count = struct.unpack_from("<2I", data, 52)
        self.supermodel = data[148:180].split(b"\0")[0].decode("latin-1")
        (self.mdx_offset, self.mdx_size, self.name_off, self.name_cnt,
         self.name_cnt2) = struct.unpack_from("<5I", data, 184)
        base = 12 + self.name_off
        self.name_offsets = (list(struct.unpack_from(f"<{self.name_cnt}I", data, base))
                             if self.name_cnt and base + self.name_cnt * 4 <= len(data)
                             else [])
        self.nodes = {}
        if self.root_offset:
            self._walk(self.root_offset, 0, set())

    def _name(self, i):
        if not self.name_offsets or i >= len(self.name_offsets):
            return ""
        o = 12 + self.name_offsets[i]
        return self.buf[o:].split(b"\0")[0].decode("latin-1")

    def _walk(self, off, depth, seen):
        if off in seen:
            raise ParseError(f"{self.name}: node cycle at {off}")
        seen.add(off)
        a = 12 + off
        if a + 80 > len(self.buf):
            raise ParseError(f"{self.name}: node header exceeds file")
        nt, ni, nni, _pad = struct.unpack_from("<4H", self.buf, a)
        parent = struct.unpack_from("<I", self.buf, a + 12)[0]
        pos = struct.unpack_from("<3f", self.buf, a + 16)
        quat = struct.unpack_from("<4f", self.buf, a + 28)
        child_arr, child_n, _child_n2 = struct.unpack_from("<3I", self.buf, a + 44)
        node = {"off": off, "type": nt, "index": ni, "name": self._name(nni),
                "parent": parent, "pos": pos, "quat": quat, "depth": depth,
                "kids": []}
        if nt in MESH_TYPES and a + 80 + 328 <= len(self.buf):
            h = a + 80
            (foff, fcnt) = struct.unpack_from("<2I", self.buf, h + 8)
            stride, vc_tex, mdx_off = struct.unpack_from("<3I", self.buf, h + 252)[0], \
                struct.unpack_from("<I", self.buf, h + 304)[0], \
                struct.unpack_from("<I", self.buf, h + 324)[0]
            node["mesh"] = {
                "face_off": foff, "face_count": fcnt,
                "stride": stride,
                "vertex_count": vc_tex & 0xFFFF,
                "tex_count": vc_tex >> 16,
                "mdx_offset": mdx_off,
                "type_name": MESH_TYPES[nt],
            }
        self.nodes[off] = node
        if child_n:
            if 12 + child_arr + child_n * 4 > len(self.buf):
                raise ParseError(f"{self.name}: child array exceeds file")
            kids = list(struct.unpack_from(f"<{child_n}I", self.buf, 12 + child_arr))
            node["kids"] = kids
            for k in kids:
                if k not in self.nodes:
                    self._walk(k, depth + 1, seen)

    def mesh_nodes(self):
        return [n for n in self.nodes.values() if "mesh" in n]


def mdx_uv1(mdl: Mdl, mdx: bytes):
    """Yield (node_name, vertex_index, u_bits, v_bits, u, v) for every
    lightmap (uv1) coordinate of every mesh with tex_count >= 2.

    uv1 lives at vertex-record offset +32 (recon/15 stride table:
    stride 40 = pos,normal,uv0,uv1).
    """
    for nd in mdl.mesh_nodes():
        m = nd["mesh"]
        if m["tex_count"] < 2 or m["vertex_count"] == 0:
            continue
        base = m["mdx_offset"]
        stride = m["stride"]
        if stride < 40:
            continue
        for i in range(m["vertex_count"]):
            off = base + i * stride + 32
            if off + 8 > len(mdx):
                raise ParseError(f"{mdl.name}: uv1 record {i} outside MDX")
            ub, vb = struct.unpack_from("<2I", mdx, off)
            u, v = struct.unpack_from("<2f", mdx, off)
            yield nd["name"], i, ub, vb, u, v


# --------------------------------------------------------------------------
# small vector/geometry helpers (RH Z-up, metres)
# --------------------------------------------------------------------------

def qmat(q):
    w, x, y, z = q
    n = w * w + x * x + y * y + z * z
    if n == 0:
        return ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
    s = 2.0 / n
    xx, yy, zz = x * x * s, y * y * s, z * z * s
    xy, xz, yz = x * y * s, x * z * s, y * z * s
    wx, wy, wz = w * x * s, w * y * s, w * z * s
    return ((1 - yy - zz, xy - wz, xz + wy),
            (xy + wz, 1 - xx - zz, yz - wx),
            (xz - wy, yz + wx, 1 - xx - yy))


def node_world(mdl: Mdl, node):
    """Accumulate parent->child T*R (no scale); recon/04 §4 verified chain."""
    chain = []
    cur = node
    seen = set()
    while cur is not None and cur["off"] not in seen:
        seen.add(cur["off"])
        chain.append(cur)
        cur = mdl.nodes.get(cur["parent"])
    R = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
    t = (0.0, 0.0, 0.0)
    for nd in reversed(chain):
        t = tuple(t[i] + sum(R[i][j] * nd["pos"][j] for j in range(3)) for i in range(3))
        q = qmat(nd["quat"])
        R = tuple(tuple(sum(R[i][k] * q[k][j] for k in range(3)) for j in range(3))
                  for i in range(3))
    return R, t


def point_in_tri2d(px, py, tri, eps=1e-9):
    """Barycentric containment in XY; returns bary tuple or None."""
    (x1, y1), (x2, y2), (x3, y3) = tri
    d = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3)
    if abs(d) < 1e-15:
        return None
    a = ((y2 - y3) * (px - x3) + (x3 - x2) * (py - y3)) / d
    b = ((y3 - y1) * (px - x3) + (x1 - x3) * (py - y3)) / d
    c = 1.0 - a - b
    if a >= -eps and b >= -eps and c >= -eps:
        return (a, b, c)
    return None


def dist_to_tri2d(px, py, tri):
    """Distance from point to triangle in XY (0 if inside)."""
    if point_in_tri2d(px, py, tri) is not None:
        return 0.0
    best = float("inf")
    for i in range(3):
        ax, ay = tri[i]
        bx, by = tri[(i + 1) % 3]
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        if L2 < 1e-30:
            d = math.hypot(px - ax, py - ay)
        else:
            t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L2))
            d = math.hypot(px - (ax + t * dx), py - (ay + t * dy))
        best = min(best, d)
    return best
