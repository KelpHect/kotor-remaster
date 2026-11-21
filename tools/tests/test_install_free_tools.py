#!/usr/bin/env python3
"""Install-free tests for tools that previously had no test at all.

Everything here is synthetic: TLK tables, PE images, a ledger and a mini game
tree are built in memory or under a temp dir.  No game data is read; where a
tool checks the retail ``swkotor.exe`` hash, the expected hash is patched to
the synthetic file's so the logic around the guard is exercised.

Run:  python tools/tests/test_install_free_tools.py
  or  python tools/tests/run_all.py
"""

from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import os
import shutil
import sqlite3
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TESTS = Path(__file__).resolve().parent
TOOLS = TESTS.parent
for _p in (TESTS, TOOLS, TOOLS / "formats", TOOLS / "inventory"):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

import kotor_types  # noqa: E402
import pe_summary  # noqa: E402
import tlk  # noqa: E402
import test_mapcompile as tm  # noqa: E402
from mapcompile import compile as mc  # noqa: E402
from viewer import area as varea  # noqa: E402
from viewer import area_viewer  # noqa: E402
from viewer import kotor as vkotor  # noqa: E402


def load_by_path(name: str, relpath: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / relpath)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


patch1280 = load_by_path("kotor_windowed_1280", "mod/kotor_windowed_1280.py")
ledger = load_by_path("evidence_ledger", "evidence/ledger.py")


class TempDirCase(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="kotor_tools_test_"))
        self.addCleanup(shutil.rmtree, self.tmp, True)


def quiet(func, *args, **kwargs):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        return func(*args, **kwargs), out.getvalue()


# ---------------------------------------------------------------------------
# synthetic builders
# ---------------------------------------------------------------------------

def tlk_bytes(entries, language=0):
    """entries: list of (text, sound_resref, flags)."""
    text_off = tlk.HEADER.size + len(entries) * tlk.ENTRY.size
    table, blob = bytearray(), bytearray()
    for text, sound, flags in entries:
        raw = text.encode("cp1252")
        table += tlk.ENTRY.pack(flags, sound.encode("latin-1"), 0, 0, len(blob), len(raw), 0.0)
        blob += raw
    return tlk.HEADER.pack(b"TLK ", b"V3.0", language, len(entries), text_off) + bytes(table) + bytes(blob)


def pe32_bytes(sections, import_rva=0, total=None):
    """sections: list of (name, vsize, va, raw_size, raw_ptr)."""
    pe = 0x40
    data = bytearray(total or max(s[3] + s[4] for s in sections))
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, pe)
    data[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, pe + 4, 0x14C, len(sections), 1_000_000_000, 0, 0, 224, 0x010F)
    opt = pe + 24
    struct.pack_into("<H", data, opt, 0x10B)
    struct.pack_into("<I", data, opt + 16, 0x1000)       # entry RVA
    struct.pack_into("<I", data, opt + 28, 0x00400000)   # image base
    struct.pack_into("<I", data, opt + 56, 0x400000)     # size of image
    struct.pack_into("<I", data, opt + 92, 16)           # data directory count
    struct.pack_into("<II", data, opt + 104, import_rva, 40 if import_rva else 0)
    for i, (name, vsize, va, raw_size, raw_ptr) in enumerate(sections):
        hdr = opt + 224 + i * 40
        data[hdr:hdr + len(name)] = name
        struct.pack_into("<IIII", data, hdr + 8, vsize, va, raw_size, raw_ptr)
        struct.pack_into("<I", data, hdr + 36, 0x60000020)
    return data


def retail_shaped_exe() -> bytes:
    """PE32 with the three 1.0.3.0 patch sites and empty trampoline padding."""
    data = pe32_bytes([(b".text", 0x1000, 0x1000, 0x33C000, 0x400)])
    for rva, orig in ((patch1280.VALIDATOR_RVA, patch1280.VALIDATOR_ORIG),
                      (patch1280.TOOLTIP_CMP_RVA, patch1280.TOOLTIP_CMP_ORIG),
                      (patch1280.HUD_BRANCH_RVA, patch1280.HUD_BRANCH_ORIG)):
        off = 0x400 + rva - 0x1000
        data[off:off + len(orig)] = orig
    return bytes(data)


def synthetic_install(root: Path) -> str:
    """Mini game tree (one module) plus a stand-in swkotor.exe; returns its hash."""
    tm.build_synthetic_game(str(root))
    # Retail BIFs store the full resource id (bif_index << 20 | table_index);
    # the mapcompile fixture stores the bare table index.  Rewrite to the
    # retail shape, which both the viewer and the mapcompile loader accept.
    ka = vkotor.ka
    for bi, entry in enumerate(ka.read_key(str(root / "chitin.key")).bifs):
        path = ka.bif_path_for(str(root), entry["name"])
        bif = ka.read_bif(path)
        ka.write_bif(path, [((bi << 20) | ti, rec.res_type, bif.read_record(rec))
                            for ti, rec in enumerate(bif.records)])
    exe = b"MZ synthetic stand-in, not a game binary"
    (root / "swkotor.exe").write_bytes(exe)
    return hashlib.sha256(exe).hexdigest()


# ---------------------------------------------------------------------------
# formats/tlk.py
# ---------------------------------------------------------------------------

class TlkReaderTests(TempDirCase):
    def write(self, data: bytes) -> str:
        path = self.tmp / "dialog.tlk"
        path.write_bytes(data)
        return str(path)

    def test_summarize_and_dump(self):
        path = self.write(tlk_bytes([("Hello", "", 1), ("", "", 0), ("Caf\u00e9 open", "snd_01", 7)], language=2))
        dump = self.tmp / "dump.jsonl"
        summary = tlk.summarize(path, str(dump))
        self.assertEqual(summary["entries"], 3)
        self.assertEqual(summary["language"], 2)
        self.assertEqual(summary["empty_strings"], 1)
        self.assertEqual(summary["entries_with_sound"], 1)
        self.assertEqual(summary["longest_text"], {"chars": 9, "strref": 2})
        self.assertEqual(summary["flag_histogram"], {0: 1, 1: 1, 7: 1})
        rows = [json.loads(line) for line in dump.read_text(encoding="utf-8").splitlines()]
        self.assertEqual([r["text"] for r in rows], ["Hello", "", "Caf\u00e9 open"])
        self.assertEqual(rows[2]["sound"], "snd_01")

    def test_rejects_wrong_magic_and_truncated_table(self):
        with self.assertRaises(ValueError):
            tlk.read_header(self.write(b"TLK V9.9" + bytes(12)))
        good = tlk_bytes([("abc", "", 0)])
        with self.assertRaises(ValueError):
            tlk.read_header(self.write(good[:30]))

    def test_rejects_text_outside_blob(self):
        path = self.write(tlk_bytes([("abc", "", 0)])[:-1])
        with self.assertRaises(ValueError):
            list(tlk.iter_entries(path, tlk.read_header(path)))

    def test_cli_exit_codes(self):
        good = self.write(tlk_bytes([("abc", "", 0)]))
        out = self.tmp / "out" / "summary.json"
        with mock.patch.object(sys, "argv", ["tlk.py", good, "--out", str(out)]):
            rc, _ = quiet(tlk.main)
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(out.read_text(encoding="utf-8"))["entries"], 1)
        bad = self.tmp / "bad.tlk"
        bad.write_bytes(b"not a talk table at all!")
        for argv, expected in ((["tlk.py", str(bad)], 1), (["tlk.py", str(bad), "--check"], 2)):
            with mock.patch.object(sys, "argv", argv):
                rc, _ = quiet(tlk.main)
            self.assertEqual(rc, expected)


# ---------------------------------------------------------------------------
# inventory/pe_summary.py and kotor_types.py
# ---------------------------------------------------------------------------

class PeSummaryTests(TempDirCase):
    def test_sections_and_imports(self):
        data = pe32_bytes([(b".text", 0x200, 0x1000, 0x200, 0x200),
                           (b".idata", 0x200, 0x2000, 0x200, 0x400)], import_rva=0x2000)
        struct.pack_into("<I", data, 0x400 + 12, 0x2080)   # descriptor 0 name RVA
        data[0x480:0x48D] = b"KERNEL32.dll\0"
        path = self.tmp / "sample.exe"
        path.write_bytes(data)
        info = pe_summary.parse(str(path))
        self.assertEqual(info["machine"], "0x14c")
        self.assertFalse(info["is_64bit"])
        self.assertEqual(info["image_base"], "0x400000")
        self.assertEqual(info["entry_rva"], "0x1000")
        self.assertEqual([s["name"] for s in info["sections"]], [".text", ".idata"])
        self.assertEqual(info["import_dlls"], ["KERNEL32.dll"])
        self.assertEqual(info["sha256"], hashlib.sha256(data).hexdigest())
        self.assertFalse(info["rich_header_present"])
        with mock.patch.object(sys, "argv", ["pe_summary.py", str(path)]):
            rc, text = quiet(pe_summary.main)
        self.assertEqual(rc, 0)
        self.assertIn("imports: KERNEL32.dll", text)

    def test_rejects_non_pe(self):
        path = self.tmp / "x.bin"
        path.write_bytes(b"ZM" + bytes(100))
        with self.assertRaises(ValueError):
            pe_summary.parse(str(path))
        path.write_bytes(b"MZ" + bytes(100))
        with self.assertRaises(ValueError):
            pe_summary.parse(str(path))

    def test_rva_to_offset(self):
        secs = [{"va": 0x1000, "vsize": 0x10, "raw_size": 0x200, "raw_ptr": 0x400}]
        self.assertEqual(pe_summary.rva_to_offset(secs, 0x1100), 0x500)
        self.assertIsNone(pe_summary.rva_to_offset(secs, 0x3000))


class ResourceTypeTableTests(unittest.TestCase):
    def test_label_and_describe(self):
        self.assertEqual(kotor_types.label(2017), "2DA")
        self.assertEqual(kotor_types.label(123456), "UNKNOWN")
        self.assertEqual(kotor_types.describe(3007), "3007:TPC (spec)")
        self.assertEqual(kotor_types.describe(123456), "123456:UNKNOWN")

    def test_table_is_well_formed(self):
        labels = [info[0] for info in kotor_types.TYPE_INFO.values()]
        self.assertEqual(len(labels), len(set(labels)))
        for rtype, (name, confidence, role) in kotor_types.TYPE_INFO.items():
            self.assertIn(confidence, {"signature", "spec", "inferred"}, rtype)
            self.assertTrue(name and role, rtype)


# ---------------------------------------------------------------------------
# mod/kotor_windowed_1280.py
# ---------------------------------------------------------------------------

class Windowed1280PatchTests(TempDirCase):
    def setUp(self):
        super().setUp()
        self.original = retail_shaped_exe()
        self.exe = self.tmp / "swkotor.exe"
        self.exe.write_bytes(self.original)
        self.digest = hashlib.sha256(self.original).hexdigest()

    def as_retail(self):
        return mock.patch.object(patch1280, "ORIGINAL_SHA256", self.digest)

    def test_refuses_unknown_executable_and_leaves_it_untouched(self):
        with self.assertRaises(SystemExit):
            patch1280.apply(self.exe)
        self.assertEqual(self.exe.read_bytes(), self.original)
        self.assertEqual(sorted(p.name for p in self.tmp.iterdir()), ["swkotor.exe"])
        self.assertEqual(patch1280.check(self.exe)["status"], "unknown")

    def test_apply_check_revert_round_trip(self):
        with self.as_retail():
            self.assertEqual(patch1280.check(self.exe)["status"], "original")
            manifest = patch1280.apply(self.exe)
            patched = self.exe.read_bytes()
            self.assertEqual(manifest["original_sha256"], self.digest)
            self.assertEqual(manifest["patched_sha256"], hashlib.sha256(patched).hexdigest())
            backup = self.tmp / "swkotor.exe.orig-1.0.3.0"
            self.assertEqual(backup.read_bytes(), self.original)
            status = patch1280.check(self.exe)
            self.assertEqual(status["status"], "patched")
            self.assertTrue(status["backup_exists"])

            pe = patch1280.PE(patched)
            self.assertEqual(pe.section(".text")["vs"], patch1280.TEXT_VS_NEW)
            for site, cave in ((patch1280.VALIDATOR_RVA, patch1280.VALIDATOR_CAVE_RVA),
                               (patch1280.TOOLTIP_CMP_RVA, patch1280.TOOLTIP_CAVE_RVA),
                               (patch1280.HUD_BRANCH_RVA, patch1280.HUD_CAVE_RVA)):
                jump = pe.get_bytes(site, 5)
                self.assertEqual(jump[:1], b"\xe9")
                self.assertEqual(site + 5 + struct.unpack("<i", jump[1:])[0], cave)
            # only the section header, the three sites and the caves changed
            changed = [i for i, (a, b) in enumerate(zip(self.original, patched)) if a != b]
            lo = pe.rva_to_off(patch1280.VALIDATOR_CAVE_RVA)
            hi = pe.rva_to_off(patch1280.CAVE_AREA_END)
            sites = {pe.rva_to_off(r) + k for r in (patch1280.VALIDATOR_RVA, patch1280.TOOLTIP_CMP_RVA,
                                                    patch1280.HUD_BRANCH_RVA) for k in range(5)}
            vs_field = {pe.section(".text")["hdr"] + 8 + k for k in range(4)}
            self.assertTrue(all(lo <= i < hi or i in sites or i in vs_field for i in changed))

            with self.assertRaises(SystemExit):   # a patched file is not the original
                patch1280.apply(self.exe)
            self.assertEqual(patch1280.revert(self.exe)["status"], "original")
        self.assertEqual(self.exe.read_bytes(), self.original)
        self.assertFalse((self.tmp / "swkotor.exe.windowed1280.json").exists())

    def test_caves_fit_and_jump_back_into_original_code(self):
        v, t, h = (patch1280.build_validator_cave(), patch1280.build_tooltip_cave(),
                   patch1280.build_hud_cave())
        self.assertLessEqual(patch1280.VALIDATOR_CAVE_RVA + len(v), patch1280.TOOLTIP_CAVE_RVA)
        self.assertLessEqual(patch1280.TOOLTIP_CAVE_RVA + len(t), patch1280.HUD_CAVE_RVA)
        self.assertLessEqual(patch1280.HUD_CAVE_RVA + len(h), patch1280.CAVE_AREA_END)
        for cave, base, target in ((v, patch1280.VALIDATOR_CAVE_RVA, patch1280.VALIDATOR_RESUME_RVA),
                                   (t, patch1280.TOOLTIP_CAVE_RVA, patch1280.TOOLTIP_FALLBACK_RVA),
                                   (h, patch1280.HUD_CAVE_RVA, patch1280.HUD_FALLBACK_RVA)):
            self.assertEqual(cave[-5:-4], b"\xe9")
            self.assertEqual(base + len(cave) + struct.unpack("<i", cave[-4:])[0], target)

    def test_refuses_when_trampoline_area_is_in_use(self):
        data = bytearray(self.original)
        data[0x400 + patch1280.HUD_CAVE_RVA - 0x1000] = 0x90
        self.exe.write_bytes(data)
        with mock.patch.object(patch1280, "ORIGINAL_SHA256", hashlib.sha256(data).hexdigest()):
            with self.assertRaises(SystemExit):
                patch1280.apply(self.exe)
        self.assertEqual(self.exe.read_bytes(), bytes(data))

    def test_revert_needs_a_genuine_backup(self):
        with self.assertRaises(SystemExit):
            patch1280.revert(self.exe)
        (self.tmp / "swkotor.exe.orig-1.0.3.0").write_bytes(b"tampered")
        with self.as_retail(), self.assertRaises(SystemExit):
            patch1280.revert(self.exe)
        self.assertEqual(self.exe.read_bytes(), self.original)

    def test_pe_rejects_other_images(self):
        with self.assertRaises(ValueError):
            patch1280.PE(bytes(0x200))
        pe = patch1280.PE(self.original)
        with self.assertRaises(ValueError):
            pe.section(".rdata")
        with self.assertRaises(ValueError):
            pe.rva_to_off(0x7FFFFFF0)


# ---------------------------------------------------------------------------
# evidence/ledger.py
# ---------------------------------------------------------------------------

class EvidenceLedgerTests(TempDirCase):
    def setUp(self):
        super().setUp()
        self.inventory = self.tmp / "inventory.json"
        self.inventory.write_text(json.dumps({
            "game_dir": "X:/synthetic",
            "manifest": [{"name": "swkotor.exe", "sha256": "ab" * 32, "size": 10,
                          "mtime_utc": "2000-01-01T00:00:00+00:00"}],
            "resources": {"by_type": {"2017": 3, "424242": 1}, "by_type_bytes": {"2017": 300}},
            "modules": {"by_type": {"2012": 1}},
            "erf_archives": [{"file": "patch.erf", "by_type": {"3007": 2}}],
        }), encoding="utf-8")
        self.actions = self.tmp / "actions.json"
        self.actions.write_text(json.dumps({"initializer": "0x1", "actions": [
            {"id": 0, "name": "Random", "handler": "00401000"},
            {"id": 1, "name": "Unused"},
        ]}), encoding="utf-8")
        self.functions = self.tmp / "functions.jsonl"
        self.functions.write_text(
            json.dumps({"entry": "00401000", "name": "FUN_00401000", "pseudocode_sha256": "cd" * 32})
            + "\n\n" + json.dumps({"entry": "00402000", "name": "FUN_00402000"}) + "\n", encoding="utf-8")
        self.db = self.tmp / "db" / "evidence.db"

    def run_cli(self, *argv):
        with mock.patch.object(sys, "argv", ["ledger.py", "--db", str(self.db), *argv]):
            rc, text = quiet(ledger.main)
        self.assertEqual(rc, 0)
        return text

    def query(self, sql):
        conn = sqlite3.connect(self.db)
        try:
            return conn.execute(sql).fetchall()
        finally:
            conn.close()

    def init(self):
        return self.run_cli("init", "--inventory", str(self.inventory), "--actions", str(self.actions),
                            "--functions", str(self.functions))

    def test_init_builds_coverage_from_synthetic_inputs(self):
        self.init()
        first = self.query("SELECT * FROM coverage")
        self.assertEqual(self.query("SELECT entry FROM functions ORDER BY entry"),
                         [("00401000",), ("00402000",)])
        self.assertEqual(self.query("SELECT source,type,label,count FROM resource_types ORDER BY source,type"),
                         [("base_bif", 2017, "2DA", 3), ("base_bif", 424242, "UNKNOWN", 1),
                          ("erf:patch.erf", 3007, "TPC", 2), ("modules", 2012, "ARE", 1)])
        self.init()   # re-running init is idempotent
        self.assertEqual(self.query("SELECT * FROM coverage"), first)

    def test_claim_is_recorded(self):
        self.init()
        before = self.query("SELECT COUNT(*) FROM claims")[0][0]
        self.run_cli("claim", "a synthetic claim", "--confidence", "derived", "--evidence", "tests")
        self.assertEqual(self.query("SELECT COUNT(*) FROM claims")[0][0], before + 1)
        self.assertEqual(self.query("SELECT claim FROM claims ORDER BY id DESC LIMIT 1"),
                         [("a synthetic claim",)])
        self.run_cli("status")

# ---------------------------------------------------------------------------
# mapcompile/compile.py and the viewer area pipeline over a synthetic install
# ---------------------------------------------------------------------------

class MapCompileCliTests(TempDirCase):
    def setUp(self):
        super().setUp()
        self.game = self.tmp / "game"
        self.digest = synthetic_install(self.game)
        self.out = self.tmp / "out"

    def run_cli(self, *argv):
        with mock.patch.object(mc, "BUILD_SHA256", self.digest):
            return quiet(mc.main, ["--game-dir", str(self.game), "--out", str(self.out), *argv])

    def test_stops_on_install_drift_before_writing(self):
        with self.assertRaises(SystemExit) as ctx:
            quiet(mc.main, ["--game-dir", str(self.game), "--out", str(self.out)])
        self.assertIn("INSTALL DRIFT", str(ctx.exception))
        self.assertFalse(self.out.exists())

    def test_compiles_every_module_and_writes_reports(self):
        rc, text = self.run_cli()
        self.assertEqual(rc, 0)
        self.assertIn("install sentinel OK", text)
        summary = json.loads((self.out / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["modules_total"], 1)
        self.assertEqual(summary["triaged_modules"], {"tmod": ["T_DOOR_NO_HOOK"]})
        self.assertEqual(summary["areas_hard_failed"], 0)
        report = json.loads((self.out / "reports" / "tmod.json").read_text(encoding="utf-8"))
        self.assertEqual((report["area"], report["status"], report["profile"]), ("t01aa", "triaged", "retail"))
        self.assertEqual(report["hard_failures"], 0)

    def test_unloadable_module_is_a_hard_failure(self):
        rc, _ = self.run_cli("--module", "tmod", "--module", "nosuch", "--profile", "strict", "--quiet")
        self.assertEqual(rc, 1)
        summary = json.loads((self.out / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["profile"], "strict")
        self.assertEqual(summary["hard_failed_modules"], {"nosuch": [tm.C.R_LOAD]})
        self.assertEqual(summary["hard_fail_exit"], 1)


class ViewerAreaTests(TempDirCase):
    def setUp(self):
        super().setUp()
        self.game_dir = self.tmp / "game"
        self.digest = synthetic_install(self.game_dir)

    def test_sentinel_stops_on_drift(self):
        with self.assertRaises(vkotor.InstallSentinelError):
            vkotor.check_install_sentinel(str(self.game_dir))
        with mock.patch.object(vkotor, "BUILD_SHA256", self.digest):
            self.assertTrue(vkotor.check_install_sentinel(str(self.game_dir))["matches_build"])

    def test_lookup_order_module_then_key(self):
        game = vkotor.Game(str(self.game_dir))
        game.use_module("tmod")
        self.assertEqual(game.find("t01aa", vkotor.T_ARE)[1], "modules/tmod.rim")
        self.assertIsNone(game.find("nosuch", vkotor.T_ARE))
        self.assertIsNone(game.find_texture("nosuch"))

    def test_load_area_and_stats(self):
        scene = varea.load_area(vkotor.Game(str(self.game_dir)), "tmod")
        self.assertEqual(scene["area"], "t01aa")
        self.assertTrue(all(c["ok"] for c in scene["checks"]), scene["checks"])
        stats = varea.scene_stats(scene)
        self.assertEqual(stats["rooms"], 1)
        self.assertEqual(stats["textures_loaded"], 0)

    def test_load_area_reports_missing_module(self):
        with self.assertRaises(ValueError):
            varea.load_area(vkotor.Game(str(self.game_dir)), "nosuch")

    def test_topdown_render(self):
        scene = varea.load_area(vkotor.Game(str(self.game_dir)), "tmod")
        img, checks = area_viewer.render_topdown(scene, 64, 48)
        self.assertEqual((img.w, img.h), (64, 48))
        self.assertTrue(all(c.get("ok") is not False for c in checks), checks)


if __name__ == "__main__":
    unittest.main(verbosity=2)
