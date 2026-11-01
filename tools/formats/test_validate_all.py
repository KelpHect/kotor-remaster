#!/usr/bin/env python3
"""End-to-end test for tools/formats/validate_all.py on a synthetic
mini-install (no game data).  The exe-hash gate is monkeypatched to the
synthetic exe; everything else runs for real."""

from __future__ import annotations

import hashlib
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "resources"))

import two_da  # noqa: E402
import two_da_ascii  # noqa: E402
import tlk_write  # noqa: E402
import kotor_archive as ka  # noqa: E402
import kotor_gff_write as gw  # noqa: E402

import validate_all  # noqa: E402
import lyt  # noqa: E402
import vis  # noqa: E402
import tga  # noqa: E402
import txi  # noqa: E402
import ssf  # noqa: E402
import wav  # noqa: E402
import lip  # noqa: E402
from test_bwm import build_bwm  # noqa: E402
from test_mdl import build_mdl  # noqa: E402
from test_bik import build as build_bik  # noqa: E402


def _lyt_bytes() -> bytes:
    body = ("#MAXLAYOUT ASCII\r\nfiledependancy t.max\r\nbeginlayout\r\n"
            "   roomcount 1\r\n      r01 1.0 2.0 0.0\r\n"
            "   trackcount 0\r\n   obstaclecount 0\r\n   doorhookcount 1\r\n"
            "      r01 Door_01 0 1.5 2.5 -0.04 1.0 0.0 0.0 0.0\r\n"
            "donelayout\r\n")
    b = body.encode("latin-1")
    while len(b) % 4:
        b += b"#x\r\n"
    return b


def _vis_bytes() -> bytes:
    name = "r01"
    while True:
        b = (name + " 1" + chr(13) + chr(10) + "  " + name + chr(13) + chr(10)).encode("latin-1")
        if len(b) % 4 == 0:
            return b
        name += "x"


def _two_da_bytes() -> bytes:
    t = two_da.TwoDA(
        version="V2.b",
        columns=("label", "a", "b"),
        rows=(two_da.Row("0", (two_da.Cell("0"), two_da.Cell("xxxx"), two_da.Cell("yy"))),),
    )
    blob = two_da_ascii.write_2da_v2b(t)
    while len(blob) % 4:
        t.rows[0].cells[1].text += "x"
        blob = two_da_ascii.write_2da_v2b(t)
    return blob


def _gff_bytes(magic=b"ARE ") -> bytes:
    for k in range(4):
        root = gw.GffStruct(0xFFFFFFFF)
        root.add("Name", "locstring", {"strref": 7, "substrings": [(0, "area")]})
        root.add("Tag", "string", "t")
        root.add("Pad", "binary", b"p" * k)
        def make_child():
            c = gw.GffStruct(100)
            c.add("X", "float", 1.5)
            return c
        root.add("Props", "struct", make_child())
        root.add("List", "list", [make_child()])
        blob = gw.gff_bytes(magic, root, b"V3.2")
        if len(blob) % 4 == 0:
            return blob
    raise AssertionError("no %4 GFF size")


def _pth_bytes() -> bytes:
    root = gw.GffStruct(0xFFFFFFFF)
    p = gw.GffStruct(0)
    p.add("X", "float", 1.0).add("Y", "float", 2.0)
    p.add("First_Conection", "uint32", 0).add("Conections", "uint32", 1)
    c = gw.GffStruct(0)
    c.add("Destination", "uint32", 0)
    root.add("Path_Points", "list", [p])
    root.add("Path_Conections", "list", [c])
    return gw.gff_bytes(b"PTH ", root, b"V3.2")  # GFF sections are u32-multiple


def _gui_bytes() -> bytes:
    for k in range(4):  # binary pad length cycles the total size mod 4
        root = gw.GffStruct(0xFFFFFFFF)
        root.add("CONTROLTYPE", "int32", 0)
        root.add("TAG", "string", "scr")
        c = gw.GffStruct(0)
        c.add("CONTROLTYPE", "int32", 1)
        c.add("TAG", "string", "btn" + "x" * k)  # last fdata entry sets total mod 4
        c.add("CONTROLS", "list", [])
        root.add("CONTROLS", "list", [c])
        blob = gw.gff_bytes(b"GUI ", root, b"V3.2")
        if len(blob) % 4 == 0:
            return blob
    raise AssertionError("no %4 GUI size")


def _tga_bytes() -> bytes:
    h = bytearray(18)
    h[2] = 2  # RGB raw
    struct.pack_into("<4H", h, 8, 0, 0, 2, 1)
    h[16] = 24
    return bytes(h) + bytes(6)  # 24 bytes total


def _txi_bytes() -> bytes:
    return b"mipmap 00 \r\n"  # 12 bytes


def _ssf_bytes() -> bytes:
    return ssf.MAGIC + struct.pack("<I", ssf.LEAD) + struct.pack("<41I", *([1] * 41))


def _wav_bytes() -> bytes:
    fmt = struct.pack("<HHIIHH", 1, 1, 22050, 44100, 2, 16)
    body = b"fmt " + struct.pack("<I", len(fmt)) + fmt \
        + b"data" + struct.pack("<I", 200) + bytes(200)
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body


def _tpc_bytes() -> bytes:
    hdr = bytearray(128)
    struct.pack_into("<I", hdr, 0, 8)  # DXT1 4x4 mip0
    struct.pack_into("<f", hdr, 4, 1.0)
    struct.pack_into("<HH", hdr, 8, 4, 4)
    hdr[0x0C] = 2
    hdr[0x0D] = 1
    return bytes(hdr) + bytes(8) + _txi_bytes()  # 148 bytes


def _tlk_bytes() -> bytes:
    entries = [tlk_write.TlkEntry(1, b"s".ljust(16, b"\x00"), 0, 0, 0, 5, 1.0),
               tlk_write.TlkEntry(0, b"".ljust(16, b"\x00"), 0, 0, 5, 3, 0.0)]
    blob = b"helloabc"
    text_offset = tlk_write.HEADER.size + 2 * tlk_write.ENTRY.size + 4
    out = bytearray(tlk_write.HEADER.pack(tlk_write.MAGIC, tlk_write.VERSION, 0, 2,
                                          text_offset))
    for e in entries:
        out += tlk_write._entry_bytes(e)
    out += b"\x00" * (text_offset - len(out)) + blob
    return bytes(out)


def _lip_bytes() -> bytes:
    return lip.MAGIC + struct.pack("<fI", 1.0, 2) + struct.pack("<fB", 0.0, 1) \
        + struct.pack("<fB", 1.0, 2)


def build_install(root: str) -> str:
    gd = os.path.join(root, "game")
    os.makedirs(os.path.join(gd, "data"))
    os.makedirs(os.path.join(gd, "modules"))
    os.makedirs(os.path.join(gd, "rims"))
    os.makedirs(os.path.join(gd, "lips"))
    os.makedirs(os.path.join(gd, "movies"))
    os.makedirs(os.path.join(gd, "streamwaves"))
    # exe (hash monkeypatched in the test)
    with open(os.path.join(gd, "swkotor.exe"), "wb") as f:
        f.write(b"MZ" + bytes(62))
    resources = [
        ("classes", 2017, _two_da_bytes()),
        ("m01aa", 3000, _lyt_bytes()),
        ("m01aa", 3001, _vis_bytes()),
        ("m01aa_01a", 2016, build_bwm()),
        ("chrome1", 3, _tga_bytes()),
        ("m01aa_01a_lm0", 2022, _txi_bytes()),
        ("c_test", 2060, _ssf_bytes()),
        ("snd_test", 4, _wav_bytes()),
        ("a_test_001", 2002, build_mdl()),
        ("m01aa", 2012, _gff_bytes(b"ARE ")),
        ("m01aa", 3003, _pth_bytes()),
        ("testgui", 2047, _gui_bytes()),
    ]
    for name, rtype, data in resources:
        assert len(data) % 4 == 0, (name, rtype, len(data))
    ka.write_key_bif(gd, resources)
    with open(os.path.join(gd, "dialog.tlk"), "wb") as f:
        f.write(_tlk_bytes())
    ka.write_erf(os.path.join(gd, "patch.erf"),
                 [("tex1", 3007, _tpc_bytes())],
                 build_year=104, build_day=47, description_strref=123)
    ka.write_rim(os.path.join(gd, "modules", "end_m01aa.rim"),
                 [("m01aa", 2012, _gff_bytes(b"ARE "))])
    ka.write_rim(os.path.join(gd, "rims", "global.rim"),
                 [("c_test2", 2060, _ssf_bytes())])
    ka.write_erf(os.path.join(gd, "lips", "end_m01aa_loc.mod"),
                 [("lip1", 3004, _lip_bytes())])
    with open(os.path.join(gd, "movies", "01x.bik"), "wb") as f:
        f.write(build_bik())
    with open(os.path.join(gd, "streamwaves", "s01.wav"), "wb") as f:
        f.write(_wav_bytes())
    return gd


class TestValidateAll(unittest.TestCase):
    def test_synthetic_install_exit0(self):
        with tempfile.TemporaryDirectory() as tmp:
            gd = build_install(tmp)
            exe_hash = hashlib.sha256(open(os.path.join(gd, "swkotor.exe"), "rb").read()).hexdigest()
            old = validate_all.EXPECTED_EXE_SHA256
            validate_all.EXPECTED_EXE_SHA256 = exe_hash
            out = os.path.join(tmp, "validate_all.json")
            try:
                rc = validate_all.main(["--game-dir", gd, "--out", out])
            finally:
                validate_all.EXPECTED_EXE_SHA256 = old
            self.assertEqual(rc, 0, open(out).read()[:4000])
            with open(out) as f:
                res = json.load(f)
            self.assertTrue(res["exe_matches_build_of_record"])
            self.assertTrue(res["summary"]["all_ok"])
            rt = res["roundtrip"]
            self.assertEqual(rt["gff_reencode"]["count"],
                             rt["gff_reencode"]["byte_identical"])
            self.assertGreaterEqual(rt["gff_reencode"]["count"], 3)
            self.assertEqual(rt["two_da_v2b"]["count"], 1)
            self.assertEqual(rt["two_da_v2b"]["byte_identical"], 1)
            self.assertEqual(rt["two_da_v2b"]["ascii_semantic_equal"], 1)
            self.assertEqual(rt["tlk"]["entries"], 2)
            self.assertEqual(rt["tlk"]["byte_identical"], 1)
            self.assertEqual(rt["erf"]["byte_identical"], 1)
            self.assertEqual(rt["rim"]["byte_identical"], 2)
            self.assertEqual(rt["bif"]["byte_identical"], 1)
            self.assertTrue(rt["key"]["ok"])
            for cls in ("lyt", "vis", "wok", "tga", "txi", "ssf", "wav", "mdl",
                        "gff", "pth", "gui", "tpc", "lip", "bik", "two_da", "tlk"):
                self.assertIn(cls, res["classes"], cls)
                self.assertEqual(res["classes"][cls]["fail"], 0, cls)
            self.assertEqual(res["classes"]["lyt"]["count"], 1)
            self.assertEqual(res["classes"]["wok"]["count"], 1)
            self.assertEqual(res["classes"]["bik"]["count"], 1)

    def test_drift_stops(self):
        with tempfile.TemporaryDirectory() as tmp:
            gd = build_install(tmp)
            # do NOT patch the hash: synthetic exe must be rejected
            rc = validate_all.main(["--game-dir", gd,
                                    "--out", os.path.join(tmp, "v.json")])
            self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
