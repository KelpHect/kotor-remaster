"""Minimal MDL+MDX model parser for area room meshes.

Layout (verified on the shipped corpus; docs/recon/04 §4, docs/recon/15 and
artifacts/lanes/10-coords-open/cont/mdx2 notes, build SHA-256
0290889e...74e6).  All MDL offsets exclude the 12-byte file header
(file offset = 12 + value); MDX has no header and mesh blocks are addressed
by MDX-relative offsets.

* file header 12 B: u32 0, u32 file_len-12, u32 mdx_size
* model header @12: root node offset @+40, node count @+44,
  supermodel name 32 B @+136, name table offset @+184, count @+188,
  count2 @+192
* node @12+off (80 B header): u16 type @0, u16 name index @4 (shares the
  node-number slot), u32 parent @12, 3f position @16, 4f quat WXYZ @28,
  u32 children array offset @44, u32 child count @48, (u32 dup @52),
  controller fields @56..80
* trimesh node type 0x21, mesh header at node+80:
  u32 face-record offset @+8 (rel 12), u32 face count @+12,
  bbox min 3f @+20 / max 3f @+32, char tex0[32] @+88, char tex1[32] @+120,
  u32 vertex stride @+252, u32 vertex_count | texture_count<<16 @+304,
  u32 MDX block offset @+324
* face record 32 B at 12+face_off: 3f normal, f32 plane, u32 material,
  3u16 adjacency, 3u16 vertex indices @+26
* MDX vertex record (stride 24/32/40/...): 3f position @0, 3f normal @12,
  2f uv0 @24 when stride >= 32, 2f uv1 @32 when stride >= 40

Node world transform: accumulate parent->child
``t += R * pos; R = R * qmat(quat)`` (no scale), then add the LYT room
origin at scene level.  Quaternions are WXYZ (verified).
"""

from __future__ import annotations

import struct

TRIMESH = 0x21


def qmat(q):
    w, x, y, z = q
    n = w * w + x * x + y * y + z * z
    if n == 0.0:
        return ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
    s = 2.0 / n
    xx, yy, zz = x * x * s, y * y * s, z * z * s
    xy, xz, yz = x * y * s, x * z * s, y * z * s
    wx, wy, wz = w * x * s, w * y * s, w * z * s
    return ((1 - yy - zz, xy - wz, xz + wy),
            (xy + wz, 1 - xx - zz, yz - wx),
            (xz - wy, yz + wx, 1 - xx - yy))


def _mm(a, b):
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3))
                 for i in range(3))


def _mv(m, v):
    return tuple(sum(m[i][j] * v[j] for j in range(3)) for i in range(3))


class Mdl:
    def __init__(self, data: bytes, mdx: bytes, name: str = "<mdl>"):
        if len(data) < 12 or struct.unpack_from("<I", data, 0)[0] != 0:
            raise ValueError(f"{name}: MDL first u32 must be 0")
        self.name = name
        self.buf = data
        self.mdx = mdx
        root, node_count = struct.unpack_from("<2I", data, 12 + 40)
        self.supermodel = data[12 + 136:12 + 168].split(b"\0")[0].decode("latin-1")
        name_off, name_cnt = struct.unpack_from("<2I", data, 12 + 184)
        self.name_offsets = list(struct.unpack_from(
            "<%dI" % name_cnt, data, 12 + name_off)) if name_cnt else []
        self.node_count = node_count
        self.nodes: dict[int, dict] = {}
        if root:
            self._walk(root, 0)

    def _name(self, idx: int) -> str:
        if not self.name_offsets or idx >= len(self.name_offsets):
            return ""
        o = 12 + self.name_offsets[idx]
        return self.buf[o:].split(b"\0")[0].decode("latin-1")

    def _walk(self, off: int, depth: int):
        b = self.buf
        a = 12 + off
        if a + 80 > len(b):
            raise ValueError(f"{self.name}: node @{off} overruns file")
        ntype = struct.unpack_from("<H", b, a)[0]
        name_idx = struct.unpack_from("<H", b, a + 4)[0]
        parent = struct.unpack_from("<I", b, a + 12)[0]
        pos = struct.unpack_from("<3f", b, a + 16)
        quat = struct.unpack_from("<4f", b, a + 28)
        kids_off, kids_n = struct.unpack_from("<2I", b, a + 44)
        kids = ([struct.unpack_from("<I", b, 12 + kids_off + i * 4)[0]
                 for i in range(kids_n)] if kids_n else [])
        node = {"off": off, "type": ntype, "name": self._name(name_idx),
                "parent": parent, "pos": pos, "quat": quat,
                "kids": kids, "depth": depth}
        if ntype == TRIMESH:
            m = a + 80
            (face_off, face_n) = struct.unpack_from("<2I", b, m + 8)
            node["face_off"] = face_off
            node["face_n"] = face_n
            node["tex0"] = b[m + 88:m + 120].split(b"\0")[0].decode("latin-1")
            node["tex1"] = b[m + 120:m + 152].split(b"\0")[0].decode("latin-1")
            node["stride"] = struct.unpack_from("<I", b, m + 252)[0]
            vc_tc = struct.unpack_from("<I", b, m + 304)[0]
            node["vc"] = vc_tc & 0xFFFF
            node["tex_count"] = vc_tc >> 16
            node["mdx_off"] = struct.unpack_from("<I", b, m + 324)[0]
        self.nodes[off] = node
        for k in kids:
            if k not in self.nodes:
                self._walk(k, depth + 1)

    def node_world(self, node: dict):
        """Accumulated (R, t) for a node (parent chain, no scale)."""
        chain = []
        cur = node
        seen = set()
        while cur is not None and cur["off"] not in seen:
            seen.add(cur["off"])
            chain.append(cur)
            cur = self.nodes.get(cur["parent"])
        R = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
        t = (0.0, 0.0, 0.0)
        for nd in reversed(chain):
            t = tuple(t[i] + sum(R[i][j] * nd["pos"][j] for j in range(3))
                      for i in range(3))
            R = _mm(R, qmat(nd["quat"]))
        return R, t

    def meshes(self):
        """Yield trimesh node dicts with parsed faces/vertices (local space)."""
        for nd in self.nodes.values():
            if nd["type"] != TRIMESH:
                continue
            b = self.buf
            stride = nd["stride"]
            vc = nd["vc"]
            fo = 12 + nd["face_off"]
            faces = [struct.unpack_from("<3H", b, fo + i * 32 + 26)
                     for i in range(nd["face_n"])]
            verts = []
            uvs = []
            mo = nd["mdx_off"]
            for i in range(vc):
                o = mo + i * stride
                if o + stride > len(self.mdx):
                    raise ValueError(f"{self.name}: MDX vertex {i} overruns MDX")
                verts.append(struct.unpack_from("<3f", self.mdx, o))
                if stride >= 32:
                    uvs.append(struct.unpack_from("<2f", self.mdx, o + 24))
                else:
                    uvs.append((0.0, 0.0))
            yield {"node": nd, "faces": faces, "verts": verts, "uvs": uvs}

    def world_triangles(self, origin=(0.0, 0.0, 0.0)):
        """World-space textured triangles: (p0,p1,p2, uv0,uv1,uv2, tex0, tex1)."""
        out = []
        for mesh in self.meshes():
            nd = mesh["node"]
            R, t = self.node_world(nd)
            wv = []
            for v in mesh["verts"]:
                lv = _mv(R, v)
                wv.append((origin[0] + t[0] + lv[0],
                           origin[1] + t[1] + lv[1],
                           origin[2] + t[2] + lv[2]))
            for f in mesh["faces"]:
                p = tuple(wv[i] for i in f)
                uv = tuple(mesh["uvs"][i] for i in f)
                out.append((p, uv, nd["tex0"], nd["tex1"]))
        return out
