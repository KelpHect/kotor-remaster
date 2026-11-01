"""Minimal WOK/DWK/PWK (BWM V1.0) parser: vertices, faces, normals.

Header layout verified on shipped walkmeshes (docs/recon/04 §3,
tools/geometry/probe.py): magic ``BWM V1.0`` @0, u32 type @8 (1 = WOK with
per-face normals), then 16 u32 counts/offsets @72.  For type 1: vertex
count/offset, face count/offset, then a normal array with one vec3 per face.
WOK vertices are already world-space (verified: adjacent rooms share exact
vertices); do NOT add the LYT room origin.
"""

from __future__ import annotations

import struct


class Wok:
    def __init__(self, data: bytes, name: str = "<wok>"):
        if data[:8] != b"BWM V1.0":
            raise ValueError(f"{name}: not BWM V1.0")
        self.name = name
        self.buf = data
        self.type = struct.unpack_from("<I", data, 8)[0]
        (self.vc, self.vo, self.fc, self.fo, self._mo, self.no, self._do,
         self._ac, self._ao, self._unk, self._adjc, self._adjoff,
         self.ec, self.eo, self._pc, self._po) = struct.unpack_from("<16I", data, 72)
        self.verts = [struct.unpack_from("<3f", data, self.vo + i * 12)
                      for i in range(self.vc)]
        self.faces = [struct.unpack_from("<3I", data, self.fo + i * 12)
                      for i in range(self.fc)]
        self.normals = ([struct.unpack_from("<3f", data, self.no + i * 12)
                         for i in range(self.fc)] if self.type == 1 and self.no else [])
        self.edges = ([struct.unpack_from("<2I", data, self.eo + i * 8)
                       for i in range(self.ec)] if self.eo else [])

    def floor_faces(self, nz: float = 0.5):
        """Indices of upward-facing, non-degenerate faces."""
        out = []
        for fi, f in enumerate(self.faces):
            if self.normals and self.normals[fi][2] <= nz:
                continue
            a, b, c = (self.verts[i] for i in f)
            ar = abs((b[0] - a[0]) * (c[1] - a[1])
                     - (b[1] - a[1]) * (c[0] - a[0])) * 0.5
            if ar >= 1e-6:
                out.append(fi)
        return out
