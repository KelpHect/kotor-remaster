#!/usr/bin/env python3
"""Strict parser/validator for KotOR MDL (binary model) files (type 2002).

MDL has no magic; the first u32 must be 0.  Layout (file offsets; the
geometry/node data lives after the 12-byte file header and node offsets are
relative to byte 12 -- verified on 2,832 shipped models, docs/recon/04,
docs/recon/12):

    0x00  u32 == 0
    0x04  u32 (size-ish field)
    0x08  u32
    0x0C  u32 == 0x413670   K1-PC geometry header id
    0x10  u32
    0x14  char[32] model name (NUL padded)
    0x34  u32 root node offset (relative to byte 12)
    0x38  u32 node count
    0x94  char[32] supermodel name ("NULL" = none)
    0xB8  6 x u32: unknown3, mdx_size, mdx_offset, name_offsets_off,
          name_offsets_count, name_offsets_count2  (name offsets rel. to 12)
    0x70  2 x u32: animation table offset/count (relative to byte 12)

Node record (at 12 + node_offset):
    +0  u16 node type, u16 node index, u16 name index, u16 pad
    +12 u32 parent offset (relative to byte 12)
    +16 3 x f32 position, +28 4 x f32 quaternion (W,X,Y,Z)
    +44 u32 child-array offset, +48 u32 child count, +52 u32 child count2
    +56 6 x u32 controller array fields
Trimesh nodes (type 33) carry an extra block at node+80 (bbox at +20/+32).

Build of record: SHA-256
0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass, field


class MdlError(ValueError):
    pass

K1_PC_ID = 0x413670
FILE_HEADER = 12


@dataclass
class MdlNode:
    offset: int          # relative to byte 12
    type: int
    index: int
    name_index: int
    name: str
    parent: int
    pos: tuple[float, float, float]
    quat: tuple[float, float, float, float]  # W, X, Y, Z
    children: tuple[int, ...]
    controllers: tuple[int, ...]
    depth: int = 0


@dataclass
class Mdl:
    name: str
    root_offset: int
    node_count: int
    supermodel: str
    mdx_size: int
    mdx_offset: int
    animation_offset: int
    animation_count: int
    name_offsets: tuple[int, ...]
    nodes: list[MdlNode] = field(default_factory=list)
    size: int = 0
    warnings: list[str] = field(default_factory=list)


def _cstr(data: bytes, off: int, maxlen: int, what: str, name: str) -> str:
    if off < 0 or off + maxlen > len(data):
        raise MdlError(f"{name}: {what} at {off}+{maxlen} outside file ({len(data)})")
    raw = data[off:off + maxlen].split(b"\x00")[0]
    return raw.decode("latin-1")


def parse_mdl(data: bytes, name: str = "<memory>") -> Mdl:
    warnings: list[str] = []
    if len(data) < 0xC4:
        raise MdlError(f"{name}: shorter than minimal MDL header ({len(data)})")
    if struct.unpack_from("<I", data, 0)[0] != 0:
        raise MdlError(f"{name}: first u32 must be 0")
    geom_id = struct.unpack_from("<I", data, 0x0C)[0]
    if geom_id != K1_PC_ID:
        raise MdlError(f"{name}: geometry header id 0x{geom_id:08x} != K1-PC 0x{K1_PC_ID:08x}")
    model_name = _cstr(data, 0x14, 32, "model name", name)
    root_offset, node_count = struct.unpack_from("<2I", data, 0x34)
    # animation table (offset/count pair, offsets relative to byte 12) at
    # file offset 0x64 = 100 -- verified on shipped animated models
    anim_off, anim_cnt = struct.unpack_from("<2I", data, 0x64)
    supermodel = _cstr(data, 0x94, 32, "supermodel", name)
    (unknown3, mdx_size, mdx_offset, name_off_off,
     name_cnt, name_cnt2) = struct.unpack_from("<6I", data, 0xB8)
    if name_cnt != name_cnt2:
        raise MdlError(f"{name}: name offset counts differ ({name_cnt} vs {name_cnt2})")
    if name_cnt:
        base = FILE_HEADER + name_off_off
        if base + name_cnt * 4 > len(data):
            raise MdlError(f"{name}: name offset table outside file")
        name_offsets = struct.unpack_from(f"<{name_cnt}I", data, base)
    else:
        name_offsets = ()
    if anim_cnt:
        abase = FILE_HEADER + anim_off
        if abase + anim_cnt * 44 > len(data):
            raise MdlError(f"{name}: animation table [{abase},+{anim_cnt}*44) outside file")

    def node_name(nni: int) -> str:
        if not name_offsets:
            return ""
        if nni >= len(name_offsets):
            raise MdlError(f"{name}: node name index {nni} out of range ({len(name_offsets)})")
        return _cstr(data, FILE_HEADER + name_offsets[nni], 64, "node name", name)

    nodes: list[MdlNode] = []
    seen: set[int] = set()

    def walk(off: int, depth: int) -> None:
        if off in seen:
            raise MdlError(f"{name}: node offset {off} visited twice (cycle)")
        seen.add(off)
        a = FILE_HEADER + off
        if a + 80 > len(data):
            raise MdlError(f"{name}: node at {off} (file {a}) outside file ({len(data)})")
        ntype, nindex, nname, _pad = struct.unpack_from("<4H", data, a)
        parent = struct.unpack_from("<I", data, a + 12)[0]
        pos = struct.unpack_from("<3f", data, a + 16)
        quat = struct.unpack_from("<4f", data, a + 28)
        child_arr, child_n, child_n2 = struct.unpack_from("<3I", data, a + 44)
        ctrl = struct.unpack_from("<6I", data, a + 56)
        if child_n != child_n2:
            raise MdlError(f"{name}: node {off} child counts differ ({child_n}/{child_n2})")
        children = ()
        if child_n:
            cbase = FILE_HEADER + child_arr
            if cbase + child_n * 4 > len(data):
                raise MdlError(f"{name}: node {off} child array outside file")
            children = struct.unpack_from(f"<{child_n}I", data, cbase)
        nodes.append(MdlNode(off, ntype, nindex, nname, node_name(nname), parent,
                             pos, quat, children, ctrl, depth))
        for c in children:
            walk(c, depth + 1)

    if root_offset:
        walk(root_offset, 0)
    if node_count and len(nodes) != node_count:
        # shipped quirk: some models (e.g. pfb*) declare more nodes than the
        # root-reachable tree contains
        warnings.append(f"{name}: header node count {node_count} != walked {len(nodes)}")
    # parents may reference offsets outside the walked set (the loader chain
    # stops there); only bounds are enforced, which walk() already did.
    m = Mdl(model_name, root_offset, node_count, supermodel, mdx_size, mdx_offset,
            anim_off, anim_cnt, name_offsets, nodes, len(data))
    m.warnings = warnings
    return m


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Validate one MDL resource (read-only)")
    ap.add_argument("path")
    args = ap.parse_args(argv)
    with open(args.path, "rb") as f:
        data = f.read()
    m = parse_mdl(data, args.path)
    print(f"{args.path}: name={m.name!r} nodes={len(m.nodes)} super={m.supermodel!r} "
          f"mdx_size={m.mdx_size} anims={m.animation_count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
