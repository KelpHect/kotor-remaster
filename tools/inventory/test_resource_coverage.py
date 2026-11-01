#!/usr/bin/env python3
"""Synthetic-fixture tests for tools/inventory/resource_coverage.py (lane s1-01).

No game data is used: every archive/file is built in a temp directory with the
deterministic fixture writers from tools/resources/kotor_archive.py and the GFF
writer from tools/resources/kotor_gff_write.py.

Run from this directory:
    python -m unittest -v test_resource_coverage
or:
    python tools/inventory/test_resource_coverage.py
"""

from __future__ import annotations

import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
for sub in ("tools/inventory", "tools/resources", "tools/formats"):
    p = str(ROOT / sub)
    if p not in sys.path:
        sys.path.insert(0, p)

import kotor_archive as ka  # noqa: E402
import kotor_gff_write as gw  # noqa: E402
from kotor_types import TYPE_INFO  # noqa: E402
import resource_coverage as rc  # noqa: E402


# ---------------------------------------------------------------------------
# payload builders (synthetic; formats per docs/recon/02_TECHNICAL_NOTES.md)
# ---------------------------------------------------------------------------


def gff_payload(file_type: str, fields: list) -> bytes:
    root = gw.GffStruct(0xFFFFFFFF)
    for label, ftype, value in fields:
        root.add(label, ftype, value)
    ft = file_type.encode("latin-1").ljust(4)[:4]  # GFF magic is exactly 4 bytes
    return gw.gff_bytes(ft, root, version=b"V3.2")


def two_da_payload() -> bytes:
    return (b"2DA V2.0\n"
            b"col_a\tcol_b\n"
            b"0\t1\t2\n"
            b"1\t3\t4\n")


def two_da_v2b_tab() -> bytes:
    """V2.b with TAB-separated columns (data/2da.bif style)."""
    cols = b"label\tcol_a\t"
    head = b"2DA V2.b\n" + cols + b"\x00"
    head += struct.pack("<I", 2)          # row count
    head += b"0\t1\t"                     # labels, TAB-terminated
    head += struct.pack("<4H", 0, 2, 4, 6)  # cell offsets
    head += struct.pack("<H", 8)          # string table size
    head += b"a\x00b\x00c\x00d\x00"
    return head


def two_da_v2b_nul() -> bytes:
    """V2.b NUL-separated variant (rims/global.rim style): names and labels
    NUL-separated, column blob ends with a double NUL."""
    head = b"2DA V2.b\n" + b"label\x00col_a\x00\x00"
    head += struct.pack("<I", 2)
    head += b"0\x001\x00"
    head += struct.pack("<4H", 0, 2, 4, 6)
    head += struct.pack("<H", 8)
    head += b"a\x00b\x00c\x00d\x00"
    return head


def tlk_payload(entries: int = 2) -> bytes:
    text_off = 20 + entries * 40
    blob = b"hello\x00" + b"world!\x00"
    head = struct.pack("<4s4sIII", b"TLK ", b"V3.0", 0, entries, text_off)
    table = b""
    offs = [(0, 6), (6, 6)]
    for i in range(entries):
        off, length = offs[i]
        table += struct.pack("<I16sIIIIf", 0, b"snd", 0, 0, off, length, 1.0)
    return head + table + blob


def ncs_payload(body: bytes = b"") -> bytes:
    size = 13 + len(body)
    return b"NCS V1.0" + b"\x42" + struct.pack(">I", size) + body


def ssf_payload() -> bytes:
    return b"SSF V1.1" + struct.pack("<28I", *range(100, 128))


def lip_payload(frames: int = 3) -> bytes:
    head = b"LIP V1.0" + struct.pack("<fI", 1.5, frames)
    body = b"".join(struct.pack("<fB", i * 0.1, i % 16) for i in range(frames))
    return head + body


def bwm_payload() -> bytes:
    vc, fc = 3, 1
    vo, fo = 136, 136 + vc * 12
    head = b"BWM V1.0" + struct.pack("<I", 1)
    head += b"\x00" * (72 - len(head))
    fields = [vc, vo, fc, fo] + [0] * 12
    head += struct.pack("<16I", *fields)
    verts = b"".join(struct.pack("<3f", float(i), 0.0, 0.0) for i in range(vc))
    faces = struct.pack("<3I", 0, 1, 2)
    return head + verts + faces


def mdl_payload() -> bytes:
    b = bytearray(256)
    struct.pack_into("<I", b, 0, 0)              # first u32 must be 0
    b[20:24] = b"mdl\x00"                        # name area
    struct.pack_into("<I", b, 52, 0)             # root offset 0 = no nodes
    return bytes(b)


def mdx_payload() -> bytes:
    return struct.pack("<3I", 0, 0, 0) + b"\x00" * 64


def tga_payload(w=4, h=4, bpp=24, dtype=2, trailer=b"") -> bytes:
    head = struct.pack("<BBBHHBHHHHBB", 0, 0, dtype, 0, 0, 0, 0, 0, w, h, bpp, 0)
    return head + b"\x11" * (w * h * bpp // 8) + trailer


def wav_payload(prefix=b"") -> bytes:
    fmt = struct.pack("<HHIIHH", 1, 1, 8000, 8000, 1, 8) + b"\x00" * 0
    data = b"\x00" * 16
    body = b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    riff = b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body
    return prefix + riff


def mp3_payload() -> bytes:
    return b"BMU" + b"\xff\xfb" + b"\x00" * 32


def bik_payload() -> bytes:
    return b"BIKi" + struct.pack("<I", 100) + b"\x00" * 32


def dds_payload() -> bytes:
    # sig(4) + header-size u32 (124) + 120 bytes of header fields = 128 minimum
    return b"DDS " + struct.pack("<I", 124) + struct.pack("<II", 0x1007, 64) + b"\x00" * 112


def tpc_payload(w=4, h=4, enc=2, mips=1, data_size=None, txi=b"islightmap 1\n") -> bytes:
    if data_size is None:
        data_size = w * h  # pretend 1 byte/px payload
    head = bytearray(128)
    struct.pack_into("<I", head, 0, data_size)
    struct.pack_into("<f", head, 4, 1.0)
    struct.pack_into("<HH", head, 8, w, h)
    head[0xC] = enc
    head[0xD] = mips
    return bytes(head) + b"\x00" * data_size + txi


def lyt_payload() -> bytes:
    return (b"#MAXLAYOUT ASCII\n"
            b"filedependancy T.max\n"
            b"beginlayout\n"
            b"   roomcount 2\n"
            b"      Room0 0.0 0.0 0.0\n"
            b"      Room1 1.0 2.0 3.0\n"
            b"   doorhookcount 1\n"
            b"      Room0 Door_01 0 1.0 2.0 3.0 0.0 0.0 0.0 1.0\n"
            b"endlayout\n")


def vis_payload() -> bytes:
    return b"room0 room1\nroom2\n"


def txi_payload() -> bytes:
    return b"islightmap 1\ncompresstextures 1\n"


def txi_numeric_first() -> bytes:
    # shipped filmnoisetex.txi opens with a numeric line
    return b"0.02\nproctexscale 1.0\n"


def ini_payload() -> bytes:
    return b"[Display]\nFullScreen=0\n; comment\n"


def ltr_payload() -> bytes:
    return b"LTR V1.0" + struct.pack("<I", 3) + b"\x00" * 12


# ---------------------------------------------------------------------------
# fixture game tree
# ---------------------------------------------------------------------------


def build_fixture_tree(root: Path) -> dict:
    """Build a minimal synthetic install; returns expected counts."""
    g = root / "game"
    bif_res = [
        # (resref, type, payload)
        ("testare", 2012, gff_payload("ARE", [("Name", 10, "test"), ("Width", 4, 10)])),
        ("testifo", 2014, gff_payload("IFO", [("Mod_Name", 10, "test")])),
        ("testdlg", 2029, gff_payload("DLG", [("StartingList", 15, [])])),
        ("testpth", 3003, gff_payload("PTH", [("Path_Conections", 15, [])])),
        ("table", 2017, two_da_payload()),
        ("script", 2010, ncs_payload()),
        ("source", 2009, b"// test\nvoid main() {}\n"),
        ("texinfo", 2022, txi_payload()),
        ("layout", 3000, lyt_payload()),
        ("vis", 3001, vis_payload()),
        ("sounds", 2060, ssf_payload()),
        ("walk", 2016, bwm_payload()),
        ("doorwalk", 2052, bwm_payload()),
        ("pwalk", 2053, bwm_payload()),
        ("model", 2002, mdl_payload()),
        ("modelx", 3008, mdx_payload()),
        ("tex", 3, tga_payload()),
        ("sound", 4, wav_payload()),
        ("soundpfx", 4, wav_payload(prefix=b"\x00" * 470)),  # ~470-byte prefix quirk
        ("music", 8, mp3_payload()),
        ("packed", 3007, tpc_payload()),
        ("cfg", 7, ini_payload()),
        ("notes", 10, b"hello world\n"),
        ("letters", 2036, ltr_payload()),
        ("bti", 2024, gff_payload("BTI", [("TemplateResRef", 11, "bti1")])),
        ("btc", 2026, gff_payload("BTC", [("TemplateResRef", 11, "btc1")])),
    ]
    ka.write_key_bif(str(g), bif_res, bif_rel="data\\base.bif")

    (g / "modules").mkdir(parents=True, exist_ok=True)
    mod_res = [("m_are", 2012, gff_payload("ARE", [("Width", 4, 1)])),
               ("m_git", 2023, gff_payload("GIT", [])),
               ("m_ncs", 2010, ncs_payload(b"\x01"))]
    ka.write_rim(str(g / "modules" / "tmod.rim"), mod_res)
    (g / "rims").mkdir(parents=True, exist_ok=True)
    ka.write_rim(str(g / "rims" / "global.rim"),
                 [("g_wav", 4, wav_payload()), ("g_ssf", 2060, ssf_payload())])
    ka.write_erf(str(g / "patch.erf"),
                 [("p_tpc", 3007, tpc_payload()), ("p_gui", 2047, gff_payload("GUI", [("X", 4, 0)]))])
    (g / "TexturePacks").mkdir(parents=True, exist_ok=True)
    ka.write_erf(str(g / "TexturePacks" / "tp.erf"), [("t_tpc", 3007, tpc_payload())])
    (g / "lips").mkdir(parents=True, exist_ok=True)
    ka.write_erf(str(g / "lips" / "t_loc.mod"), [("lip1", 3004, lip_payload())], magic=b"MOD ")

    (g / "dialog.tlk").write_bytes(tlk_payload())
    for d in ("Override", "streamwaves", "streamsounds", "streammusic", "movies"):
        (g / d).mkdir(parents=True, exist_ok=True)
    (g / "streamwaves" / "a.wav").write_bytes(wav_payload())
    (g / "streamwaves" / "noext").write_bytes(wav_payload(prefix=b"\x00" * 100))  # sniffed as wav
    (g / "streamwaves" / "subdir").mkdir()
    (g / "streamwaves" / "subdir" / "b.wav").write_bytes(wav_payload())
    (g / "streamsounds" / "s.wav").write_bytes(wav_payload())
    (g / "streammusic" / "m.wav").write_bytes(wav_payload())
    (g / "movies" / "mv.bik").write_bytes(bik_payload())

    return {
        "key_bif_files": 1,
        "key_resources": len(bif_res),
        "module_rim_files": 1,
        "module_resources": 3,
        "module_entry_bytes": sum(len(p) for _, _t, p in mod_res),
        "global_rim_files": 1,
        "global_rim_resources": 2,
        "global_rim_entry_bytes": len(wav_payload()) + len(ssf_payload()),
        "patch_erf_entries": 2,
        "texture_erf_files": 1,
        "texture_erf_entries": 1,
        "loose_dialog_tlk": 1,
        "loose_streamwaves": 3,
        "loose_streamsounds": 1,
        "loose_streammusic": 1,
        "loose_movies": 1,
        "loose_lips_mods": 1,
        "loose_override": 0,
    }


def fixture_spec(counts: dict):
    cites = {name: "synthetic fixture" for name, _e, _c in rc.CROSS_CHECK_SPEC}
    return [(name, counts[name], cites[name]) for name, _e, _c in rc.CROSS_CHECK_SPEC]


class ValidatorTests(unittest.TestCase):
    """Unit tests for the header validators: good payloads pass, bad fail."""

    def setUp(self):
        self.tmp = rc._TmpFile()

    def tearDown(self):
        self.tmp.close()

    def check(self, bucket, payload, rtype=0):
        return rc.VALIDATORS[bucket](payload, rtype, self.tmp)

    def test_gff_ok_and_bad(self):
        ok = gff_payload("ARE", [("Width", 4, 7)])
        status, _ = self.check("gff", ok, 2012)
        self.assertEqual(status, "ok")
        status, detail = self.check("gff", b"ARE V3.2" + b"\x00" * 60, 2012)
        self.assertEqual(status, "failed")
        # wrong magic for the declared type
        status, _ = self.check("gff", gff_payload("IFO", [("A", 4, 1)]), 2012)
        self.assertEqual(status, "failed")

    def test_two_da(self):
        self.assertEqual(self.check("two_da", two_da_payload())[0], "ok")
        self.assertEqual(self.check("two_da", b"garbage")[0], "failed")
        # V2.b TAB variant goes through the real parser
        status, detail = self.check("two_da", two_da_v2b_tab())
        self.assertEqual(status, "ok")
        self.assertNotIn("NUL-separated", detail)
        # V2.b NUL-separated variant: strict parser rejects, header fallback ok
        status, detail = self.check("two_da", two_da_v2b_nul())
        self.assertEqual(status, "ok")
        self.assertIn("NUL-separated", detail)
        # structurally broken NUL variant must still fail
        bad = two_da_v2b_nul()
        bad = bad[:20] + struct.pack("<I", 10 ** 6) + bad[24:]
        self.assertEqual(self.check("two_da", bad)[0], "failed")

    def test_tlk(self):
        self.assertEqual(self.check("tlk", tlk_payload())[0], "ok")
        bad = bytearray(tlk_payload())
        bad[6:8] = b"V9.9"
        self.assertEqual(self.check("tlk", bytes(bad))[0], "failed")

    def test_ncs(self):
        self.assertEqual(self.check("ncs", ncs_payload())[0], "ok")
        bad = ncs_payload()
        bad = bad[:-1] + b"\x00"  # size field no longer matches
        self.assertEqual(self.check("ncs", bad)[0], "failed")
        self.assertEqual(self.check("ncs", b"NCS V1.0\x43" + struct.pack(">I", 13))[0], "failed")

    def test_ssf_lip(self):
        self.assertEqual(self.check("ssf", ssf_payload())[0], "ok")
        self.assertEqual(self.check("ssf", b"SSF V9.9" + b"\x00" * 200)[0], "failed")
        self.assertEqual(self.check("lip", lip_payload())[0], "ok")
        bad = lip_payload(3)[:-1]
        self.assertEqual(self.check("lip", bad)[0], "failed")
        bad_viseme = b"LIP V1.0" + struct.pack("<fI", 1.0, 1) + struct.pack("<fB", 0.0, 99)
        self.assertEqual(self.check("lip", bad_viseme)[0], "failed")

    def test_bwm(self):
        self.assertEqual(self.check("bwm", bwm_payload(), 2016)[0], "ok")
        bad = bytearray(bwm_payload())
        struct.pack_into("<I", bad, 72, 100000)  # vc out of range
        self.assertEqual(self.check("bwm", bytes(bad), 2016)[0], "failed")

    def test_mdl_mdx(self):
        self.assertEqual(self.check("mdl", mdl_payload())[0], "ok")
        bad = bytearray(mdl_payload())
        struct.pack_into("<I", bad, 0, 5)
        self.assertEqual(self.check("mdl", bytes(bad))[0], "failed")
        self.assertEqual(self.check("mdx", mdx_payload())[0], "ok")
        # MDX has no header: float-leading block streams are valid (observed
        # a_jedirobe_001.mdx starts with f32 -0.0948); only alignment is checked
        self.assertEqual(self.check("mdx", struct.pack("<3f", -0.0948, 0.0, -1.0) + b"\x00" * 4)[0], "ok")
        self.assertEqual(self.check("mdx", b"\x01" * 7)[0], "failed")
        self.assertEqual(self.check("mdx", b"")[0], "ok")  # empty MDX exists

    def test_tga(self):
        self.assertEqual(self.check("tga", tga_payload())[0], "ok")
        # 26 trailing bytes are tolerated (observed on shipped chrome1.tga)
        self.assertEqual(self.check("tga", tga_payload(trailer=b"\x00" * 26))[0], "ok")
        self.assertEqual(self.check("tga", tga_payload()[:-4])[0], "failed")
        bad = bytearray(tga_payload())
        bad[16] = 7  # invalid bpp
        self.assertEqual(self.check("tga", bytes(bad))[0], "failed")

    def test_wav(self):
        self.assertEqual(self.check("wav", wav_payload())[0], "ok")
        self.assertEqual(self.check("wav", wav_payload(prefix=b"\x00" * 470))[0], "ok")
        self.assertEqual(self.check("wav", b"nope" * 100)[0], "failed")

    def test_media(self):
        self.assertEqual(self.check("mp3", mp3_payload())[0], "ok")
        self.assertEqual(self.check("mp3", b"\xff\xfb\x00\x00")[0], "ok")
        self.assertEqual(self.check("mp3", b"\x00" * 16)[0], "failed")
        self.assertEqual(self.check("bik", bik_payload())[0], "ok")
        self.assertEqual(self.check("bik", b"NOPE" + b"\x00" * 16)[0], "failed")
        self.assertEqual(self.check("dds", dds_payload())[0], "ok")
        bad = bytearray(dds_payload())
        struct.pack_into("<I", bad, 4, 999)
        self.assertEqual(self.check("dds", bytes(bad))[0], "failed")

    def test_tpc(self):
        self.assertEqual(self.check("tpc", tpc_payload())[0], "ok")
        bad = bytearray(tpc_payload())
        bad[0xC] = 0x55  # unknown encoding
        self.assertEqual(self.check("tpc", bytes(bad))[0], "failed")
        bad2 = bytearray(tpc_payload())
        struct.pack_into("<I", bad2, 0, 10 ** 9)  # data size beyond file
        self.assertEqual(self.check("tpc", bytes(bad2))[0], "failed")

    def test_text_formats(self):
        self.assertEqual(self.check("lyt", lyt_payload())[0], "ok")
        self.assertEqual(self.check("lyt", b"nope\nroomcount 1\nRoom0 0 0 0\n")[0], "failed")
        bad = lyt_payload().replace(b"roomcount 2", b"roomcount 5")
        self.assertEqual(self.check("lyt", bad)[0], "failed")
        self.assertEqual(self.check("vis", vis_payload())[0], "ok")
        self.assertEqual(self.check("txi", txi_payload())[0], "ok")
        self.assertEqual(self.check("txi", txi_numeric_first())[0], "ok")
        self.assertEqual(self.check("ini", ini_payload())[0], "ok")
        self.assertEqual(self.check("ini", b"no section here\n")[0], "failed")
        self.assertEqual(self.check("text", b"void main(){}\n")[0], "ok")
        self.assertEqual(self.check("ltr", ltr_payload())[0], "ok")

    def test_probe_and_reserved(self):
        # the probe bucket still exists as the fallback for unknown layouts
        status, _ = self.check("probe", b"\x01\x02\x03\x04xxxx")
        self.assertEqual(status, "open")
        self.assertEqual(self.check("reserved", b"\x00" * 8)[0], "failed")
        # BTI/BTC payloads are GFF V3.2 (verified against the install)
        status, _ = self.check("gff", gff_payload("BTC", [("A", 4, 1)]), 2026)
        self.assertEqual(status, "ok")

    def test_erf_container_validator(self):
        blob = ka.erf_bytes([("a", 4, wav_payload())])
        self.assertEqual(self.check("erf", blob)[0], "ok")
        self.assertEqual(self.check("erf", b"XXXX" + b"\x00" * 200)[0], "failed")


class GateTests(unittest.TestCase):
    def test_every_kotor_type_has_a_bucket(self):
        """Gate G1: zero unhandled type ids in kotor_types.py."""
        self.assertEqual(rc.unhandled_type_ids(), [])
        missing = [t for t in TYPE_INFO if t not in rc.TYPE_BUCKET]
        self.assertEqual(missing, [])
        missing_v = [b for b in rc.TYPE_BUCKET.values() if b not in rc.VALIDATORS]
        self.assertEqual(missing_v, [])


class LedgerTests(unittest.TestCase):
    """End-to-end synthetic run of the enumerator over a fixture install."""

    @classmethod
    def setUpClass(cls):
        cls.tmpdir = tempfile.TemporaryDirectory()
        cls.counts = build_fixture_tree(Path(cls.tmpdir.name))
        cls.out = os.path.join(cls.tmpdir.name, "coverage.json")
        cls.ledger = rc.run(os.path.join(cls.tmpdir.name, "game"), cls.out,
                            log=lambda *a: None, verify_build=False,
                            cross_check_spec=fixture_spec(cls.counts))

    @classmethod
    def tearDownClass(cls):
        cls.tmpdir.cleanup()

    def test_ledger_file_written(self):
        with open(self.out, encoding="utf-8") as f:
            data = json.load(f)
        self.assertIn("resources", data)
        self.assertIn("summary", data)
        self.assertIn("containers", data)

    def test_cross_checks_pass(self):
        s = self.ledger["summary"]
        self.assertTrue(s["cross_checks_pass"],
                        [c for c in s["cross_checks"] if not c["pass"]])

    def test_totals(self):
        t = self.ledger["summary"]["totals"]
        # 26 key + 3 module + 2 rims + 2 patch + 1 texture + 1 lip + 7 loose
        self.assertEqual(t["resources"], 26 + 3 + 2 + 2 + 1 + 1 + 7)
        self.assertEqual(t["by_status"].get("failed", 0), 0)
        self.assertEqual(t["by_status"].get("open", 0), 0)  # BTI/BTC are GFF
        self.assertEqual(t["by_source"]["key"], 26)
        self.assertEqual(t["by_source"]["module_rim"], 3)
        self.assertEqual(t["by_source"]["global_rim"], 2)
        self.assertEqual(t["by_source"]["patch_erf"], 2)
        self.assertEqual(t["by_source"]["texture_erf"], 1)
        self.assertEqual(t["by_source"]["lips_mod"], 1)
        self.assertEqual(t["by_source"]["loose"], 7)

    def test_gate_g1_zero_unhandled(self):
        g = self.ledger["summary"]["gate_g1"]
        self.assertTrue(g["zero_unhandled"])
        self.assertEqual(g["unhandled_type_ids_in_kotor_types"], [])

    def test_every_record_has_required_fields(self):
        for r in self.ledger["resources"]:
            for field in ("source", "archive", "entry", "resref", "type",
                          "type_label", "size", "sha256", "magic", "bucket",
                          "status", "detail"):
                self.assertIn(field, r, r)
            self.assertEqual(len(r["sha256"]), 64)
            self.assertIn(r["status"], ("ok", "failed", "open"))
            self.assertGreater(r["size"], 0)

    def test_bucket_assignment_matches_type(self):
        by_resref = {r["resref"]: r for r in self.ledger["resources"]}
        self.assertEqual(by_resref["testare"]["bucket"], "gff")
        self.assertEqual(by_resref["testare"]["status"], "ok")
        self.assertEqual(by_resref["table"]["bucket"], "two_da")
        self.assertEqual(by_resref["script"]["bucket"], "ncs")
        self.assertEqual(by_resref["soundpfx"]["bucket"], "wav")
        self.assertEqual(by_resref["soundpfx"]["status"], "ok")  # prefix tolerated
        self.assertEqual(by_resref["packed"]["bucket"], "tpc")
        self.assertEqual(by_resref["walk"]["bucket"], "bwm")
        self.assertEqual(by_resref["model"]["bucket"], "mdl")
        self.assertEqual(by_resref["tex"]["bucket"], "tga")
        self.assertEqual(by_resref["bti"]["bucket"], "gff")   # BTI V3.2
        self.assertEqual(by_resref["bti"]["status"], "ok")
        self.assertEqual(by_resref["btc"]["bucket"], "gff")
        # extensionless loose wav sniffed by RIFF probe
        self.assertEqual(by_resref["noext"]["type"], 4)
        self.assertEqual(by_resref["noext"]["status"], "ok")
        # lips mod entry classified as lip
        lip = [r for r in self.ledger["resources"] if r["source"] == "lips_mod"][0]
        self.assertEqual(lip["bucket"], "lip")
        self.assertEqual(lip["status"], "ok")
        # dialog.tlk validated through the TLK bucket
        tlk = [r for r in self.ledger["resources"] if r["archive"] == "dialog.tlk"][0]
        self.assertEqual(tlk["bucket"], "tlk")
        self.assertEqual(tlk["status"], "ok")

    def test_by_type_summary_counts(self):
        bt = self.ledger["summary"]["by_type"]
        self.assertEqual(bt["2012"]["count"], 2)   # testare + m_are
        self.assertEqual(bt["2012"]["label"], "ARE")
        wavs = [r for r in self.ledger["resources"] if r["type"] == 4]
        self.assertEqual(bt["4"]["count"], len(wavs))
        self.assertEqual(bt["4"]["count"], 8)  # 2 key + 1 rim + 5 loose wav files

    def test_containers_hashed(self):
        paths = {c["path"] for c in self.ledger["containers"]}
        self.assertIn("chitin.key", paths)
        self.assertIn("data/base.bif", paths)
        self.assertIn("modules/tmod.rim", paths)
        self.assertIn("rims/global.rim", paths)
        self.assertIn("patch.erf", paths)
        self.assertIn("TexturePacks/tp.erf", paths)
        self.assertIn("lips/t_loc.mod", paths)
        self.assertIn("dialog.tlk", paths)
        for c in self.ledger["containers"]:
            self.assertEqual(len(c["sha256"]), 64)

    def test_drift_guard(self):
        # a tree whose swkotor.exe does not match the artifact of record must abort
        g = Path(self.tmpdir.name) / "game"
        (g / "swkotor.exe").write_bytes(b"not the game")
        with self.assertRaises(SystemExit):
            rc.run(str(g), os.path.join(self.tmpdir.name, "x.json"),
                   log=lambda *a: None, verify_build=True)
        (g / "swkotor.exe").unlink()


class GameDirRunTests(unittest.TestCase):
    """One read-only run against KOTOR_GAME_DIR when explicitly requested."""

    def test_real_install(self):
        gd = os.environ.get("KOTOR_GAME_DIR")
        if not gd or os.environ.get("KOTOR_COVERAGE_GAME_TEST") != "1":
            self.skipTest("set KOTOR_GAME_DIR and KOTOR_COVERAGE_GAME_TEST=1 for the "
                          "read-only install run")
        out = os.environ.get(
            "KOTOR_COVERAGE_OUT",
            os.path.join(tempfile.gettempdir(), "s1_01_coverage_real.json"))
        ledger = rc.run(gd, out, log=lambda *a: None)
        s = ledger["summary"]
        self.assertTrue(s["exe"]["matches_artifact_of_record"])
        self.assertTrue(s["gate_g1"]["zero_unhandled"])
        self.assertTrue(s["cross_checks_pass"],
                        [c for c in s["cross_checks"] if not c["pass"]])
        self.assertEqual(s["totals"]["by_status"].get("failed", 0), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
