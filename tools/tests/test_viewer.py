#!/usr/bin/env python3
"""Lane s1-08 viewer tests — synthetic fixtures only, no game data.

Covers the pure-Python building blocks of tools/viewer: PNG writer, software
rasterizer (flat/textured/z-buffer/blit), TPC (DXT1/DXT5/RGBA/gray) and TGA
decoders, LYT/WOK/MDL+MDX parsers, GUI GFF parsing/rendering, and the
game-dir path normalization.  Fixtures are built in memory with struct.
"""

from __future__ import annotations

import os
import struct
import sys
import unittest
import zlib
from pathlib import Path

TESTS = Path(__file__).resolve().parent
REPO_ROOT = TESTS.parents[1]
TOOLS = REPO_ROOT / "tools"
for _p in (str(TOOLS), str(REPO_ROOT / "tools" / "resources")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from viewer import game_dir  # noqa: E402
from viewer import gffx, gui, lyt, mdl, pngwrite, raster, tpc, wok  # noqa: E402
from viewer.pngwrite import Image, encode_png, write_png  # noqa: E402
import kotor_gff_write as gw  # noqa: E402


# --------------------------------------------------------------------------
# synthetic payload builders
# --------------------------------------------------------------------------

def build_tpc(w: int, h: int, enc: int, mips: int, payload: bytes,
              data_size: int | None = None, alpha: float = 1.0) -> bytes:
    hdr = bytearray(128)
    struct.pack_into("<I", hdr, 0, len(payload) if data_size is None else data_size)
    struct.pack_into("<f", hdr, 4, alpha)
    struct.pack_into("<2H", hdr, 8, w, h)
    hdr[12] = enc
    hdr[13] = mips
    return bytes(hdr) + payload + b"mipmap 0 \r\n"


def dxt1_block(c0: int, c1: int, bits: int) -> bytes:
    return struct.pack("<HHI", c0, c1, bits)


def build_dxt1_tpc(w=8, h=8) -> bytes:
    # 2x2 blocks; block(0,0): red vs blue, all index 0; block(1,0): index 3 etc.
    red, blue = 0xF800, 0x001F
    payload = (dxt1_block(red, blue, 0x00000000)      # all color0 (red)
               + dxt1_block(red, blue, 0xFFFFFFFF)      # all color3
               + dxt1_block(red, blue, 0x55555555)      # all color1
               + dxt1_block(blue, red, 0xFFFFFFFF))     # c0<c1, all idx3 -> transparent
    return build_tpc(w, h, 2, 1, payload, data_size=len(payload))


def build_dxt5_tpc(w=4, h=4) -> bytes:
    a0, a1 = 255, 0
    abits = 0  # all pixels alpha index 0 -> 255
    color = dxt1_block(0xF800, 0x07E0, 0)  # 4-color mode, all color0
    return build_tpc(w, h, 4, 1, bytes([a0, a1]) + abits.to_bytes(6, "little") + color,
                     data_size=16)


def build_rgba_tpc(w=2, h=2) -> bytes:
    payload = bytes([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 9, 8, 7, 6])
    return build_tpc(w, h, 4, 1, payload, data_size=len(payload))


def build_gray_tpc(w=4, h=1) -> bytes:
    return build_tpc(w, h, 1, 1, bytes([10, 20, 30, 40]), data_size=4)


def build_tga(w=4, h=2, bpp=24, img_type=2, top_down=True, pixels=None) -> bytes:
    hdr = bytearray(18)
    hdr[2] = img_type
    struct.pack_into("<2H", hdr, 12, w, h)
    hdr[16] = bpp
    hdr[17] = 0x20 if top_down else 0x00
    if pixels is None:
        pixels = bytes(range(w * h * (bpp // 8)))
    return bytes(hdr) + pixels


def build_tga_rle(w=4, h=1) -> bytes:
    hdr = bytearray(build_tga(w, h, 24, 10, pixels=b""))
    # one RLE packet of 4 pixels: BGR (1,2,3)
    return bytes(hdr) + bytes([0x83]) + bytes([3, 2, 1])


def build_wok() -> bytes:
    # BWM V1.0 type 1: 3 verts, 1 face, 1 normal
    counts = [0] * 16
    vc, vo = 3, 136
    fc, fo = 1, 172
    no = 184
    counts[0], counts[1] = vc, vo
    counts[2], counts[3] = fc, fo
    counts[5] = no
    head = b"BWM V1.0" + struct.pack("<I", 1) + b"\0" * 60 + struct.pack("<16I", *counts)
    assert len(head) == 136
    verts = struct.pack("<9f", 0, 0, 0, 4, 0, 0, 0, 3, 0)
    faces = struct.pack("<3I", 0, 1, 2)
    normal = struct.pack("<3f", 0, 0, 1)
    return head + verts + faces + normal


def build_mdl_mdx():
    """One root node + one trimesh child with a single triangle."""
    mdx = bytearray()
    for pos, uv in (((0.0, 0.0, 0.0), (0.0, 0.0)),
                    ((1.0, 0.0, 0.0), (1.0, 0.0)),
                    ((0.0, 1.0, 0.0), (0.0, 1.0))):
        mdx += struct.pack("<3f3f2f", *pos, 0, 0, 1, *uv)  # stride 32
    mdx = bytes(mdx)

    buf = bytearray(1024)
    # file header
    struct.pack_into("<3I", buf, 0, 0, 0, len(mdx))  # patched later
    # model header @12
    struct.pack_into("<2I", buf, 12 + 40, 200, 2)          # root off (rel12), node count
    buf[12 + 136:12 + 140] = b"NULL"                        # supermodel
    struct.pack_into("<3I", buf, 12 + 184, 700, 2, 2)       # name table off/count/dup
    # name strings at file 712+... name table at 12+700=712: two u32 offsets (rel12)
    struct.pack_into("<2I", buf, 12 + 700, 720, 740)
    buf[12 + 720:12 + 725] = b"root\0"
    buf[12 + 740:12 + 746] = b"mesh\0"
    # root node @ off 200 (file 212)
    a = 12 + 200
    struct.pack_into("<2H", buf, a, 1, 0)                   # type 1, name idx 0
    struct.pack_into("<I", buf, a + 12, 0xFFFFFFFF)         # parent
    struct.pack_into("<3f", buf, a + 16, 0, 0, 0)
    struct.pack_into("<4f", buf, a + 28, 1, 0, 0, 0)
    struct.pack_into("<3I", buf, a + 44, 400, 1, 1)          # children @400, count 1
    struct.pack_into("<I", buf, 12 + 400, 500)              # child off
    # trimesh node @ off 500 (file 512)
    b = 12 + 500
    struct.pack_into("<2H", buf, b, 0x21, 1)                # type 0x21, name idx 1
    struct.pack_into("<I", buf, b + 12, 200)                # parent = root
    struct.pack_into("<3f", buf, b + 16, 5, 0, 0)           # pos
    struct.pack_into("<4f", buf, b + 28, 1, 0, 0, 0)        # quat WXYZ identity
    struct.pack_into("<3I", buf, b + 44, 0, 0, 0)           # no children
    m = b + 80
    struct.pack_into("<2I", buf, m + 8, 600, 1)             # face_off (rel12), face count
    buf[m + 88:m + 93] = b"texA\0"                          # tex0
    struct.pack_into("<I", buf, m + 252, 32)                # stride
    struct.pack_into("<I", buf, m + 304, 3 | (1 << 16))     # vc=3, tex count=1
    struct.pack_into("<I", buf, m + 324, 0)                 # mdx block offset
    # face record @ file 12+600=612: 32 bytes, vi at +26
    fr = 12 + 600
    struct.pack_into("<3f", buf, fr, 0, 0, 1)
    struct.pack_into("<3H", buf, fr + 26, 0, 1, 2)
    struct.pack_into("<I", buf, 4, len(buf) - 12)
    return bytes(buf), mdx


def build_gui_gff() -> bytes:
    def control(tag, ctype, left, top, wdt, hgt, fill=""):
        c = gw.GffStruct(0)
        c.add("CONTROLTYPE", "int32", ctype)
        c.add("TAG", "string", tag)
        ext = gw.GffStruct(0)
        ext.add_many(("LEFT", "int32", left), ("TOP", "int32", top),
                     ("WIDTH", "int32", wdt), ("HEIGHT", "int32", hgt))
        c.add("EXTENT", "struct", ext)
        border = gw.GffStruct(0)
        border.add_many(("CORNER", "resref", ""), ("EDGE", "resref", ""),
                        ("FILL", "resref", fill), ("FILLSTYLE", "int32", 2),
                        ("COLOR", "vector3", (1.0, 1.0, 1.0)))
        c.add("BORDER", "struct", border)
        return c

    root = gw.GffStruct(0xFFFFFFFF)
    root.add("CONTROLS", "list", [
        control("PANEL", 2, 0, 0, 640, 480, fill="bluefill"),
        control("BTN_OK", 6, 285, 222, 210, 22),
    ])
    return gw.gff_bytes(b"GUI ", root)


# --------------------------------------------------------------------------
# tests
# --------------------------------------------------------------------------

class TestPng(unittest.TestCase):
    def test_encode_signature_and_ihdr(self):
        img = Image(4, 3, 3, fill=(10, 20, 30))
        data = encode_png(img)
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        ln = struct.unpack(">I", data[8:12])[0]
        self.assertEqual(data[12:16], b"IHDR")
        w, h, bd, ct = struct.unpack(">IIBB", data[16:26])
        self.assertEqual((w, h, bd, ct), (4, 3, 8, 2))
        # IDAT decodes to h * (1 + w*3) bytes
        pos = 8
        idat = b""
        while pos < len(data):
            n = struct.unpack(">I", data[pos:pos + 4])[0]
            tag = data[pos + 4:pos + 8]
            if tag == b"IDAT":
                idat += data[pos + 8:pos + 8 + n]
            pos += 12 + n
        raw = zlib.decompress(idat)
        self.assertEqual(len(raw), 3 * (1 + 4 * 3))
        self.assertTrue(all(raw[y * 13] == 0 for y in range(3)))

    def test_write_png_file(self):
        import tempfile
        img = Image(2, 2, 4, fill=(1, 2, 3, 255))
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "x.png")
            n = write_png(p, img)
            self.assertEqual(n, os.path.getsize(p))
            with open(p, "rb") as f:
                self.assertEqual(f.read(8), b"\x89PNG\r\n\x1a\n")


class TestRaster(unittest.TestCase):
    def test_tri_flat_counts_and_z(self):
        img = Image(16, 16, 3, fill=(0, 0, 0))
        z = raster.new_zbuf(16, 16)
        n = raster.tri_flat(img, z, [(2, 2, 5.0), (13, 2, 5.0), (2, 13, 5.0)], (255, 0, 0))
        self.assertGreater(n, 30)
        self.assertEqual(img.get(3, 3), (255, 0, 0))
        self.assertEqual(img.get(14, 14), (0, 0, 0))
        # nearer triangle wins the z test
        raster.tri_flat(img, z, [(2, 2, 1.0), (13, 2, 1.0), (2, 13, 1.0)], (0, 255, 0))
        self.assertEqual(img.get(3, 3), (0, 255, 0))
        # farther triangle loses
        raster.tri_flat(img, z, [(2, 2, 9.0), (13, 2, 9.0), (2, 13, 9.0)], (0, 0, 255))
        self.assertEqual(img.get(3, 3), (0, 255, 0))

    def test_tri_tex_samples_texture(self):
        tex = Image(2, 2, 3)
        tex.set(0, 0, (255, 0, 0))
        tex.set(1, 0, (0, 255, 0))
        tex.set(0, 1, (0, 0, 255))
        tex.set(1, 1, (255, 255, 0))
        img = Image(16, 16, 3, fill=(0, 0, 0))
        z = raster.new_zbuf(16, 16)
        n = raster.tri_tex(img, z, [(0, 0, 1.0), (15, 0, 1.0), (0, 15, 1.0)],
                           [(0.0, 0.0), (1.9, 0.0), (0.0, 1.9)], tex)
        self.assertGreater(n, 30)
        self.assertIn(img.get(2, 2)[0], (255, 0))  # left half is red-ish

    def test_tri_tex_alpha_zero_skips(self):
        tex = Image(2, 2, 4)
        tex.set(0, 0, (255, 0, 0), 0)
        tex.set(1, 0, (255, 0, 0), 0)
        tex.set(0, 1, (255, 0, 0), 0)
        tex.set(1, 1, (255, 0, 0), 0)
        img = Image(8, 8, 3, fill=(7, 7, 7))
        z = raster.new_zbuf(8, 8)
        n = raster.tri_tex(img, z, [(0, 0, 1.0), (7, 0, 1.0), (0, 7, 1.0)],
                           [(0, 0), (1, 0), (0, 1)], tex)
        self.assertEqual(img.get(2, 2), (7, 7, 7))

    def test_blit_stretch(self):
        tex = Image(2, 2, 3)
        tex.set(0, 0, (255, 0, 0)); tex.set(1, 0, (0, 255, 0))
        tex.set(0, 1, (0, 0, 255)); tex.set(1, 1, (255, 255, 0))
        img = Image(4, 4, 3, fill=(0, 0, 0))
        n = raster.blit_stretch(img, tex, 0, 0, 4, 4)
        self.assertEqual(n, 16)
        self.assertEqual(img.get(0, 0), (255, 0, 0))
        self.assertEqual(img.get(3, 0), (0, 255, 0))
        self.assertEqual(img.get(0, 3), (0, 0, 255))
        self.assertEqual(img.get(3, 3), (255, 255, 0))


class TestTpc(unittest.TestCase):
    def test_dxt1_decode(self):
        img, meta = tpc.decode_tpc(build_dxt1_tpc())
        self.assertEqual((img.w, img.h), (8, 8))
        self.assertEqual(meta["decoded_format"], "dxt1")
        self.assertTrue(meta["header_dims_ok"])
        self.assertEqual(img.get(0, 0), (255, 0, 0, 255))       # block0 all c0 red
        # block(1,0) bits all 1 -> color3 = (c0 + 2*c1)/3 with c0=red > c1=blue
        r, g, b = (255 + 2 * 0) // 3, (0 + 2 * 0) // 3, (0 + 2 * 255) // 3
        self.assertEqual(img.get(7, 0), (r, g, b, 255))
        # block(2,0) bits 0x5555... -> color1 (blue)
        self.assertEqual(img.get(0, 4)[2], 255)
        # block(1,1): c0=blue < c1=red, all bits 3 -> transparent black
        self.assertEqual(img.get(7, 7), (0, 0, 0, 0))

    def test_dxt5_decode(self):
        img, meta = tpc.decode_tpc(build_dxt5_tpc())
        self.assertEqual(meta["decoded_format"], "dxt5")
        self.assertEqual(img.get(0, 0), (255, 0, 0, 255))

    def test_rgba_raw(self):
        img, meta = tpc.decode_tpc(build_rgba_tpc())
        self.assertEqual(meta["decoded_format"], "rgba")
        self.assertEqual(img.get(0, 0), (255, 0, 0, 255))
        self.assertEqual(img.get(1, 1), (9, 8, 7, 6))

    def test_gray(self):
        img, meta = tpc.decode_tpc(build_gray_tpc())
        self.assertEqual(meta["decoded_format"], "gray")
        self.assertEqual(img.get(2, 0), (30, 30, 30, 255))

    def test_rgb_disambiguation(self):
        w = h = 2
        payload = bytes([255, 0, 0] * 4)
        data = build_tpc(w, h, 2, 1, payload, data_size=w * h * 3)
        img, meta = tpc.decode_tpc(data)
        self.assertEqual(meta["decoded_format"], "rgb")
        self.assertEqual(img.get(1, 1), (255, 0, 0, 255))


class TestTga(unittest.TestCase):
    def test_tga24_bottom_up(self):
        # 2x1, bpp 24, bottom-up: first stored pixel is bottom row
        px = bytes([1, 2, 3, 4, 5, 6])  # BGR bottom, BGR top
        img, meta = tpc.decode_tga(build_tga(2, 1, 24, 2, top_down=False, pixels=px))
        self.assertEqual((img.w, img.h), (2, 1))
        self.assertEqual(img.get(0, 0), (3, 2, 1, 255))  # BGR -> RGB
        self.assertTrue(meta["header_dims_ok"])

    def test_tga_rle(self):
        img, meta = tpc.decode_tga(build_tga_rle(4, 1))
        self.assertEqual(meta["image_type"], 10)
        for x in range(4):
            self.assertEqual(img.get(x, 0), (1, 2, 3, 255))


class TestLytWok(unittest.TestCase):
    def test_lyt_parse(self):
        text = ("#MAXLAYOUT ASCII\nroomcount 1\nroomA 1.0 2.0 3.0\n"
                "doorhookcount 1\nroomA Door_01 attach 4.0 5.0 6.0 1.0 0.0 0.0 0.0\n"
                "donelayout\n")
        rooms, hooks = lyt.parse_lyt_text(text)
        self.assertEqual(rooms, [{"room": "roomA", "x": 1.0, "y": 2.0, "z": 3.0}])
        self.assertEqual(hooks[0]["node"], "Door_01")
        self.assertEqual(hooks[0]["quat"], (1.0, 0.0, 0.0, 0.0))

    def test_wok_parse(self):
        w = wok.Wok(build_wok(), "t.wok")
        self.assertEqual(w.type, 1)
        self.assertEqual(len(w.verts), 3)
        self.assertEqual(w.faces, [(0, 1, 2)])
        self.assertEqual(w.normals[0], (0.0, 0.0, 1.0))
        self.assertEqual(w.floor_faces(), [0])


class TestMdl(unittest.TestCase):
    def test_mdl_mdx_parse_and_transform(self):
        data, mdx = build_mdl_mdx()
        m = mdl.Mdl(data, mdx, "t.mdl")
        self.assertEqual(m.node_count, 2)
        self.assertEqual(m.supermodel, "NULL")
        meshes = list(m.meshes())
        self.assertEqual(len(meshes), 1)
        self.assertEqual(meshes[0]["node"]["tex0"], "texA")
        self.assertEqual(meshes[0]["faces"], [(0, 1, 2)])
        self.assertEqual(len(meshes[0]["verts"]), 3)
        self.assertEqual(meshes[0]["uvs"][1], (1.0, 0.0))
        tris = m.world_triangles(origin=(10.0, 20.0, 30.0))
        self.assertEqual(len(tris), 1)
        (p, uv, t0, t1) = tris[0]
        self.assertEqual(t0, "texA")
        # child node pos (5,0,0) + root identity -> vertex0 at origin+(5,0,0)
        self.assertEqual(p[0], (15.0, 20.0, 30.0))
        self.assertEqual(p[1], (16.0, 20.0, 30.0))

    def test_quat_rotation(self):
        # 90 deg about Z: (w,x,y,z) = (cos45, 0,0, sin45) maps +X to +Y
        R = mdl.qmat((2 ** 0.5 / 2, 0, 0, 2 ** 0.5 / 2))
        v = mdl._mv(R, (1.0, 0.0, 0.0))
        self.assertAlmostEqual(v[0], 0.0, places=6)
        self.assertAlmostEqual(v[1], 1.0, places=6)


class TestGui(unittest.TestCase):
    def test_parse_and_render_synthetic_gui(self):
        data = build_gui_gff()
        g = gffx.gff_from_bytes(data, "test.gui")
        self.assertEqual(gffx.gff.check(g), [])
        parsed = gui.parse_gui(data, "test")
        self.assertEqual(parsed["control_count"], 2)
        self.assertEqual(parsed["controls"][0]["extent"],
                         {"LEFT": 0, "TOP": 0, "WIDTH": 640, "HEIGHT": 480})
        self.assertEqual(parsed["controls"][0]["border_fill"], "bluefill")
        self.assertEqual(parsed["controls"][1]["control_type_name"], "button")

        class NullTex:
            cache = {}
            log = []

            def get(self, resref):
                return None

        from viewer import gui_viewer
        img, checks = gui_viewer.render_gui(parsed, NullTex(), scale=1)
        self.assertEqual((img.w, img.h), (640, 480))
        by_name = {c["name"]: c for c in checks}
        self.assertTrue(by_name["gui_controls_rendered"]["ok"])
        self.assertEqual(by_name["gui_controls_rendered"]["actual"], 2)


class TestPaths(unittest.TestCase):
    def test_game_dir_gitbash_form(self):
        self.assertEqual(game_dir("/e/SteamLibrary/steamapps/common/swkotor"),
                         "E:/SteamLibrary/steamapps/common/swkotor")
        self.assertEqual(game_dir("E:\\SteamLibrary\\x"), "E:/SteamLibrary/x")
        self.assertEqual(game_dir(None), game_dir(""))  # default when unset


if __name__ == "__main__":
    unittest.main()
