"""Sprint-1 VFS discriminating-asset fixtures (lane s1-05) closing the items
still marked inferred/open in docs/recon/10_SAVE_VFS.md.

Each test builds a synthetic discriminating tree with ``kotor_archive``
writers and asserts the resolver rule.  Rules already measured in-process on
2026-10-24 (docs/recon/08_VFS_DYNAMIC.md, ``artifacts/lanes/06-dynamic/
vfs-precedence/runs2/``) are pinned here synthetically.  The fresh-module
area-RIM mount order is no longer open: the shipped mounter at
``FUN_004c44d0`` probes ``.mod`` first, then mounts ``MODULES:<mod>_s`` before
LIPS and the optional ``_a``/``_adx`` RIMs (docs/recon/23 #23-#25).  Only real
streamwaves audio requests still need lane s1-06 dynamic measurement; that item
is listed in ``OPEN_FOR_S1_06``.

The two resolver defaults that contradicted the 2026-10-24 measurements
(flat/OS-order directory scan; measured pass-4 order) were fixed in
``kotor_vfs.py`` by the integrator; the tests below now pin the measured
defaults:

* ``DirectoryTable`` defaults to a flat, one-level, OS-order (unsorted) scan
  (08: "Loose directories are flat, one level", ``runs2/enum-dirs-1``).
* ``Vfs`` pass 4 is built in the measured search order: save > .mod >
  LIPS:localization > HD0:players > swpc_tex_gui > quality tier pack
  (08/10/22).

Build under test: SHA-256 0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6.
"""
from __future__ import annotations

import unittest
from pathlib import Path

import support
import kotor_archive as ka
from kotor_vfs import (Vfs, DirectoryTable, ArchiveTable, KeyTable, Pass,
                       normalize_resref)

# Items that still need lane s1-06 dynamic measurement.  The fresh-module
# area-RIM order was closed statically (docs/recon/23 #23-#25), so only the
# per-file streamwaves audio request remains.
OPEN_FOR_S1_06 = [
    "streamwaves audio requests before gameplay (directory and pass-1 index "
    "measured; actual audio lookups unobserved)",
]


def build_game(root: Path) -> Path:
    """Synthetic mini-install with discriminating keys per tier."""
    game = root / "game"
    for sub in ("Override", "streamwaves", "streammusic", "movies", "rims",
                "modules", "lips", "TexturePacks"):
        (game / sub).mkdir(parents=True, exist_ok=True)
    # pass 1: loose directories; 'flat_probe' exists only in a SUBDIRECTORY of
    # streamwaves to discriminate flat vs recursive indexing
    (game / "Override" / "ov_probe.tga").write_bytes(b"OVERRIDE")
    (game / "streamwaves" / "sub").mkdir()
    (game / "streamwaves" / "sub" / "only_sub.wav").write_bytes(b"SUBDIR_ONLY")
    (game / "streamwaves" / "flat_probe.wav").write_bytes(b"TOPLEVEL")
    # pass 2: patch.erf with a duplicate key (first must win)
    ka.write_erf(game / "patch.erf", [
        ("dup_erf", 2017, b"ERF_FIRST"),
        ("dup_erf", 2017, b"ERF_SECOND"),
        ("p2_probe", 2017, b"PATCH"),
    ])
    # pass 3: global.rim + module RIMs; shared key everywhere
    ka.write_rim(game / "rims" / "global.rim", [
        ("shared", 2010, b"GLOBAL_RIM"),
        ("dup_rim", 2010, b"RIM_FIRST"),
        ("dup_rim", 2010, b"RIM_SECOND"),
    ])
    for name, tag in (("active.rim", b"AREA_RIM"), ("active_a.rim", b"A_RIM"),
                      ("active_adx.rim", b"ADX_RIM"), ("active_s.rim", b"S_RIM")):
        ka.write_rim(game / "modules" / name, [("shared", 2010, tag)])
    # pass 4: .mod, lips, players-like erf, texture packs, all sharing 'shared'
    ka.write_erf(game / "modules" / "active.mod", [("shared", 2010, b"MOD"),
                                                    ("mod_only", 2011, b"MODONLY")])
    ka.write_erf(game / "lips" / "localization.mod", [("shared", 2010, b"LIPS")])
    ka.write_erf(game / "players.mod", [("shared", 2010, b"PLAYERS")])
    ka.write_erf(game / "TexturePacks" / "swpc_tex_tpa.erf",
                 [("shared", 2010, b"TPA"), ("tex_only", 3007, b"TEX")])
    ka.write_erf(game / "TexturePacks" / "swpc_tex_gui.erf",
                 [("gui_only", 3007, b"GUI")])
    # pass 5: chitin.key with a duplicate KEY record + one BIF
    (game / "data").mkdir(parents=True, exist_ok=True)
    ka.write_bif(game / "data" / "base.bif", [(0, 2017, b"BASE0"), (1, 2010, b"BASE1")])
    ka.write_key(game / "chitin.key",
                 [{"name": "data\\base.bif", "size": (game / "data" / "base.bif").stat().st_size}],
                 [("dup_key", 2017, 0, 0), ("dup_key", 2017, 0, 1),
                  ("base_probe", 2010, 0, 1)])
    # loaded save: nested module ERF for 'active' with a shared key + duplicate
    nested = ka.erf_bytes([("shared", 2010, b"SAVE"), ("dup_sav", 2010, b"SAVE_FIRST"),
                            ("dup_sav", 2010, b"SAVE_SECOND")], magic=b"MOD ")
    save_dir = game / "saves" / "000000 - TEST"
    save_dir.mkdir(parents=True)
    (save_dir / "SAVEGAME.sav").write_bytes(
        ka.erf_bytes([("active", 2057, nested)], magic=b"MOD "))
    return game


class VfsP1Fixture:
    @classmethod
    def setUpClass(cls):
        cls.tmp = support.clean_tmp("vfs-p1")
        cls.game = build_game(cls.tmp)
        cls.save = cls.game / "saves" / "000000 - TEST"

    def vfs(self, **kw) -> Vfs:
        kw.setdefault("quality", "tpa")
        return Vfs(self.game, **kw)


class DuplicateKeyTests(VfsP1Fixture, unittest.TestCase):
    def test_key_duplicate_first_record_wins(self):
        kt = KeyTable("chitin.key", self.game / "chitin.key", self.game)
        hit = kt.lookup(("dup_key", 2017))
        self.assertIsNotNone(hit)
        # first KEY record (table_index 0) wins; second AddKey returned 0 in
        # the measured run (08: KEY duplicates, byte-patched chitin.key)
        self.assertEqual(hit.entry.index, 0)
        self.assertEqual(hit.duplicate_indices, [0, 1])
        self.assertEqual(kt.read(hit.entry), b"BASE0")

    def test_erf_duplicate_first_entry_wins(self):
        t = ArchiveTable("patch.erf", self.game / "patch.erf", self.game)
        hit = t.lookup(("dup_erf", 2017))
        self.assertEqual(hit.entry.index, 0)
        self.assertEqual(hit.duplicate_indices, [0, 1])
        self.assertEqual(t.read(hit.entry), b"ERF_FIRST")

    def test_rim_duplicate_first_entry_wins(self):
        t = ArchiveTable("global.rim", self.game / "rims" / "global.rim", self.game)
        hit = t.lookup(("dup_rim", 2010))
        self.assertEqual(hit.entry.index, 1)  # first of the two dup records
        self.assertEqual(hit.duplicate_indices, [1, 2])
        self.assertEqual(t.read(hit.entry), b"RIM_FIRST")

    def test_save_nested_duplicate_first_wins(self):
        v = self.vfs(active_module="active", loaded_save=self.save)
        save_table = v.passes[3].tables[0]
        self.assertTrue(save_table.name.startswith("SAVEGAME.sav"))
        hit = save_table.lookup(("dup_sav", 2010))
        self.assertEqual(hit.entry.index, 1)  # SAVE_FIRST is the first dup entry
        self.assertEqual(save_table.read(hit.entry), b"SAVE_FIRST")
        # measured: the conversion to currentgame/<module>.rim drops the
        # duplicate (08 runs2/save-dup-1); the table itself keeps first-wins
        res = v.trace("dup_sav", 2010)
        self.assertEqual(res.winner.table.name, save_table.name)


class ModRimSaveTests(VfsP1Fixture, unittest.TestCase):
    def test_rim_beats_mod(self):
        # measured 2026-10-24 (08): a mounted RIM beats .mod because pass 3
        # precedes pass 4; on the loaded-save path the engine mounts .mod
        # INSTEAD of _s (no _s loader) - modeled here by dropping _s.
        v = self.vfs(active_module="active")
        res = v.trace("shared", 2010)
        self.assertEqual(res.winner.pass_no, 3)
        self.assertEqual(res.winner.table.name, "active_s.rim")
        self.assertEqual(res.read_payload(), b"S_RIM")

    def test_save_beats_mod_but_loses_to_s_rim(self):
        # measured (08): save nested ERF > .mod (same pass 4, save first),
        # but _s RIM (pass 3) beats the save; save only shadows stored keys
        v = self.vfs(active_module="active", loaded_save=self.save)
        res = v.trace("shared", 2010)
        self.assertEqual(res.winner.table.name, "active_s.rim")
        self.assertEqual(res.read_payload(), b"S_RIM")
        # without any RIM mounted (loaded-save path mounts no _s), the save
        # wins over .mod within pass 4
        v.passes[2].tables.clear()
        res = v.trace("shared", 2010)
        self.assertEqual(res.winner.pass_no, 4)
        self.assertTrue(res.winner.table.name.startswith("SAVEGAME.sav"))
        self.assertEqual(res.read_payload(), b"SAVE")
        res = v.trace("mod_only", 2011)
        self.assertEqual(res.winner.table.name, "active.mod")

    def test_pass4_measured_order(self):
        # measured pass-4 order: save > .mod > LIPS:localization > players >
        # texture packs (last-added-first within the pass).  Reorder the
        # resolver's pass-4 tables into the measured order and peel tiers off.
        v = self.vfs(active_module="active", loaded_save=self.save,
                     include_texture_packs=True, extra_erfs=["players.mod"])
        p4 = v.passes[3]
        measured = ["SAVEGAME.sav#active", "active.mod", "localization.mod",
                    "players.mod", "swpc_tex_tpa.erf", "swpc_tex_gui.erf"]
        ordered = [t for name in measured for t in p4.tables if t.name == name]
        p4.tables = ordered + [t for t in p4.tables if t not in ordered]
        v.passes[2].tables.clear()  # isolate pass 4 (RIMs would win first)
        self.assertEqual(v.trace("shared", 2010).winner.table.name,
                         "SAVEGAME.sav#active")
        p4.tables.remove(next(t for t in p4.tables
                              if t.name == "SAVEGAME.sav#active"))
        self.assertEqual(v.trace("shared", 2010).winner.table.name, "active.mod")
        p4.tables.remove(next(t for t in p4.tables if t.name == "active.mod"))
        self.assertEqual(v.trace("shared", 2010).winner.table.name, "localization.mod")
        p4.tables.remove(next(t for t in p4.tables if t.name == "localization.mod"))
        self.assertEqual(v.trace("shared", 2010).winner.table.name, "players.mod")
        p4.tables.remove(next(t for t in p4.tables if t.name == "players.mod"))
        self.assertEqual(v.trace("shared", 2010).winner.table.name, "swpc_tex_tpa.erf")

    def test_pass4_default_order_matches_measurement(self):
        # Integrator fix verified: the default pass-4 table order now IS the
        # measured order save > .mod > LIPS:localization > players >
        # swpc_tex_gui > tier pack (08/10/22).
        v = self.vfs(active_module="active", loaded_save=self.save,
                     include_texture_packs=True, extra_erfs=["players.mod"])
        v.passes[2].tables.clear()  # isolate pass 4
        names = [t.name for t in v.passes[3].tables]
        self.assertLess(names.index("localization.mod"), names.index("players.mod"))
        self.assertLess(names.index("players.mod"), names.index("swpc_tex_gui.erf"))
        self.assertLess(names.index("swpc_tex_gui.erf"), names.index("swpc_tex_tpa.erf"))
        for t in list(v.passes[3].tables):
            if t.name.startswith("SAVEGAME.sav") or t.name == "active.mod":
                v.passes[3].tables.remove(t)
        res = v.trace("shared", 2010)
        self.assertEqual(res.winner.table.name, "localization.mod")  # measured rule


class LooseDirTests(VfsP1Fixture, unittest.TestCase):
    def test_flat_one_level_scan_matches_measurement(self):
        # measured (08): FindFirstFile scans the loose dir once, one level;
        # a resource present only in a subdirectory is never indexed
        flat = DirectoryTable("streamwaves", self.game / "streamwaves",
                              self.game, recursive=False)
        keys = {e.key for e in flat.entries}
        self.assertIn(("flat_probe", 4), keys)
        self.assertNotIn(("only_sub", 4), keys)  # subdir-only copy invisible
        self.assertEqual(len([e for e in flat.entries if e.key == ("flat_probe", 4)]), 1)

    def test_flat_one_level_default_matches_measurement(self):
        # Integrator fix verified: the default DirectoryTable is flat, one
        # level, unsorted (OS order) - subdirectory-only resources are NOT
        # indexed (08 runs2/enum-dirs-1).  recursive=True remains available
        # only as an explicit opt-in.
        rec = DirectoryTable("streamwaves", self.game / "streamwaves",
                             self.game, recursive=True)
        self.assertIn(("only_sub", 4), {e.key for e in rec.entries})
        v = self.vfs()
        p1_names = [t.name for t in v.passes[0].tables]
        self.assertEqual(p1_names, ["Override", "movies", "streamwaves", "streammusic"])
        sw = next(t for t in v.passes[0].tables if t.name == "streamwaves")
        self.assertFalse(any(e.resref == "only_sub" for e in sw.entries))

    def test_pass1_last_added_first(self):
        # measured (08): pass-1 tables are searched last-added-first; the
        # engine added Override, MOVIES, STREAMWAVES, STREAMMUSIC in that
        # order and STREAMMUSIC won the shared border2 key.  The resolver
        # default now implements exactly this (integrator fix).
        (self.game / "streammusic" / "ov_probe.tga").write_bytes(b"MUSIC")
        try:
            v = self.vfs()
            res = v.trace("ov_probe", 3)
            self.assertEqual(res.winner.pass_no, 1)
            self.assertEqual(res.winner.table.name, "streammusic")
        finally:
            (self.game / "streammusic" / "ov_probe.tga").unlink()


class OpenItemsTests(VfsP1Fixture, unittest.TestCase):
    def test_s1_06_open_items_are_explicit(self):
        # only the real streamwaves audio-request observation still needs
        # lane s1-06; the fresh-module area-RIM order is statically closed.
        self.assertEqual(len(OPEN_FOR_S1_06), 1)
        self.assertTrue(any("streamwaves audio" in s for s in OPEN_FOR_S1_06))
        self.assertFalse(any("area-RIM" in s for s in OPEN_FOR_S1_06))
        # the resolver still mounts all four area RIMs for an active module;
        # the static mounter proves the search order _s > _adx > _a > area.
        v = self.vfs(active_module="active")
        names = [t.name for t in v.passes[2].tables]
        for suffix in ("active_s.rim", "active_adx.rim", "active_a.rim", "active.rim"):
            self.assertIn(suffix, names)
        self.assertLess(names.index("active_s.rim"), names.index("active_adx.rim"))
        self.assertLess(names.index("active_adx.rim"), names.index("active_a.rim"))
        self.assertLess(names.index("active_a.rim"), names.index("active.rim"))
        self.assertTrue(any("proven" in n for n in v.passes[2].notes))
        self.assertFalse(any("inferred" in n for n in v.passes[2].notes))


if __name__ == "__main__":
    unittest.main()
