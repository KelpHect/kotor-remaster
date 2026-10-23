from __future__ import annotations

import unittest

import support
from kotor_vfs import Vfs


SAVE = support.VFS_GAME / "saves" / "000002 - TEST"


def fixture_vfs(**kwargs) -> Vfs:
    support.ensure_fixtures()
    return Vfs(support.VFS_GAME, active_module="active", loaded_save=SAVE, **kwargs)


def winner(vfs: Vfs, resref: str, res_type: int):
    res = vfs.trace(resref, res_type)
    return res, (res.winner.table.name if res.winner else None), res.read_payload()


class VfsFixtureTests(unittest.TestCase):
    def test_directory_tier(self):
        v = fixture_vfs()
        res, table, payload = winner(v, "tier_probe", 3)
        self.assertEqual(table, "Override")
        self.assertEqual(payload, b"OVERRIDE_TIER")
        self.assertEqual(res.winner.pass_no, 1)
        self.assertEqual(winner(v, "music_probe", 4)[1], "streammusic")
        self.assertEqual(winner(v, "movie_probe", 2063)[1], "movies")

    def test_pass2_beats_pass3_and_pass4(self):
        v = fixture_vfs()
        self.assertEqual(winner(v, "patch_vs_rim", 2010)[2], b"PATCH_VS_RIM")
        self.assertEqual(winner(v, "patch_vs_rim", 2010)[1], "patch.erf")
        # Measured engine order: textures.erf loads first, patch.erf second, so
        # patch.erf is searched first (last-added-first).
        self.assertEqual(winner(v, "patch_vs_tex", 3007)[1], "patch.erf")
        self.assertEqual(winner(v, "tex_erf_probe", 3007)[1], "textures.erf")
        self.assertEqual(winner(v, "patch_only", 2017)[1], "patch.erf")

    def test_pass3_rims_and_mod_loses_to_rim(self):
        v = fixture_vfs()
        self.assertEqual(winner(v, "global_probe", 2010)[1], "global.rim")
        self.assertEqual(winner(v, "rim_vs_bif", 2017)[1], "global.rim")
        # Active module RIMs outrank the pass-4 .mod.
        self.assertEqual(winner(v, "mod_vs_rim", 2017)[1], "active_s.rim")
        self.assertEqual(winner(v, "mod_vs_rim", 2017)[2], b"STATIC_RIM")
        # Intra-pass order is inferred: static RIM before area RIM.
        self.assertEqual(winner(v, "area_vs_static", 2010)[2], b"STATIC_RIM")

    def test_pass4_texture_module_save(self):
        v = fixture_vfs()
        self.assertEqual(winner(v, "tex_probe", 3007)[1], "swpc_tex_tpa.erf")
        self.assertEqual(winner(v, "mod_vs_texture", 3007)[1], "active.mod")
        self.assertEqual(winner(v, "mod_only", 2011)[2], b"MOD_ONLY")
        self.assertEqual(winner(v, "gui_tex_probe", 3007)[1], "swpc_tex_gui.erf")
        self.assertEqual(winner(v, "lips_probe", 2029)[1], "localization.mod")
        self.assertEqual(winner(v, "lips_loc_probe", 2029)[1], "active_loc.mod")
        # Save table shadows module .mod but not pass-3 RIMs, and only stores
        # resources it actually contains.
        self.assertEqual(winner(v, "save_only", 2010)[2], b"SAVE_ERF")
        self.assertEqual(winner(v, "mod_vs_save", 2017)[2], b"SAVE_ERF")
        self.assertEqual(winner(v, "save_vs_rim", 2010)[2], b"STATIC_RIM")
        self.assertEqual(winner(v, "mod_vs_bif", 2017)[1], "active.mod")

    def test_pass5_base(self):
        v = fixture_vfs()
        self.assertEqual(winner(v, "base_probe", 2017)[1], "chitin.key")
        self.assertEqual(winner(v, "base_probe", 2017)[2], b"BASE_TIER")

    def test_duplicate_keys_first_wins(self):
        v = fixture_vfs()
        res, table, payload = winner(v, "dup_probe", 2017)
        self.assertEqual(table, "patch.erf")
        self.assertEqual(payload, b"DUP_FIRST")
        self.assertGreaterEqual(len(res.winner.duplicate_indices), 2)
        res, table, payload = winner(v, "dup_rim", 2010)
        self.assertEqual(table, "global.rim")
        self.assertEqual(payload, b"RIM_FIRST")
        self.assertGreaterEqual(len(res.winner.duplicate_indices), 2)

    def test_case_insensitive_and_case_variant_duplicate(self):
        v = fixture_vfs()
        self.assertEqual(winner(v, "CASE_PROBE", 2017)[2], b"PATCH_CASE")
        self.assertEqual(winner(v, "CaSe_PrObE", 2017)[2], b"PATCH_CASE")
        res, table, payload = winner(v, "RIMCASE", 2017)
        self.assertEqual(table, "global.rim")
        self.assertEqual(payload, b"RIM_CASE_FIRST")
        self.assertGreaterEqual(len(res.winner.duplicate_indices), 2)

    def test_same_resref_different_types_are_distinct(self):
        v = fixture_vfs()
        self.assertEqual(winner(v, "dual_probe", 3)[2], b"DUAL_TGA")
        self.assertEqual(winner(v, "dual_probe", 3007)[2], b"DUAL_TPC")

    def test_quality_selection_is_explicit_and_assumption_is_noted(self):
        auto = fixture_vfs(quality="auto")
        res = auto.trace("quality_probe", 3007)
        self.assertEqual(res.winner.table.name, "swpc_tex_tpa.erf")
        self.assertTrue(any("mapping unverified" in n for n in res.notes))
        self.assertEqual(winner(fixture_vfs(quality="tpb"), "quality_probe", 3007)[1],
                         "swpc_tex_tpb.erf")
        self.assertEqual(winner(fixture_vfs(quality="tpc"), "quality_probe", 3007)[1],
                         "swpc_tex_tpc.erf")
        self.assertEqual(winner(fixture_vfs(quality="all"), "quality_probe", 3007)[1],
                         "swpc_tex_tpa.erf")
        self.assertEqual(winner(fixture_vfs(quality="none"), "tex_probe", 3007)[1],
                         "chitin.key")

    def test_first_launch_wav_quirk(self):
        normal = fixture_vfs(first_launch=False)
        self.assertEqual(winner(normal, "wav_probe", 4)[1], "Override")
        self.assertEqual(winner(normal, "wav_probe", 4)[2], b"OVERRIDE_WAV")
        launch = fixture_vfs(first_launch=True)
        self.assertEqual(winner(launch, "wav_probe", 4)[1], "streamwaves")
        self.assertEqual(winner(launch, "wav_probe", 4)[2], b"STREAMWAVES_WAV")

    def test_trace_order_and_shadowing(self):
        v = fixture_vfs()
        res = v.trace("tier_probe", 3)
        self.assertEqual(res.traces[0].pass_no, 1)
        self.assertEqual([h.pass_no for h in res.traces], sorted(h.pass_no for h in res.traces))
        self.assertGreaterEqual(len(res.traces), 5)
        self.assertEqual(res.traces[-1].pass_no, 5)
        self.assertEqual(res.winner.table.name, "Override")
        info = res.info(include_hash=True)
        self.assertEqual(info["winner"]["table"], "Override")
        self.assertEqual(info["found"], True)
        self.assertEqual(info["payload_size"], len(b"OVERRIDE_TIER"))

    def test_not_found(self):
        v = fixture_vfs()
        res = v.trace("no_such_resource", 2017)
        self.assertFalse(res.found)
        self.assertIsNone(res.winner)
        self.assertEqual(res.traces, [])

    def test_mounts_are_five_passes(self):
        v = fixture_vfs()
        mounts = v.mounts()
        self.assertEqual([p["pass"] for p in mounts["passes"]], [1, 2, 3, 4, 5])
        self.assertEqual(mounts["passes"][4]["name"], "key-and-bifs")
        self.assertEqual(mounts["passes"][3]["tables"][0]["name"],
                         "SAVEGAME.sav#active")
        self.assertTrue(any("inferred" in n for n in mounts["notes"]))


class VfsRealInstallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (support.REAL_GAME / "chitin.key").exists():
            raise unittest.SkipTest("reference install not present")

    def test_real_archives_precedence(self):
        v = Vfs(support.REAL_GAME, active_module="m12ab", quality="tpa")
        res = v.trace("1280x1024back", 3007)
        self.assertTrue(res.found)
        self.assertEqual(res.winner.pass_no, 2)
        self.assertEqual(res.winner.table.name, "patch.erf")
        res = v.trace("m12ab", 2012)
        self.assertEqual(res.winner.pass_no, 3)
        self.assertEqual(res.winner.table.name, "m12ab.rim")
        res = v.trace("k_pebo_skybox", 2010)
        self.assertEqual(res.winner.pass_no, 3)
        self.assertEqual(res.winner.table.name, "m12ab_s.rim")
        res = v.trace("3dgui_a00001", 3007)
        self.assertEqual(res.winner.pass_no, 4)
        self.assertEqual(res.winner.table.name, "swpc_tex_tpa.erf")

    def test_real_base_only_resource_reaches_pass5(self):
        v = Vfs(support.REAL_GAME, active_module="m12ab", quality="none")
        earlier = set()
        for p in v.passes[:4]:
            for t in p.tables:
                if t.exists:
                    earlier.update(e.key for e in t.entries)
        base = v.passes[4].tables[0]
        chosen = None
        for e in base.entries:
            if e.key not in earlier:
                chosen = e
                break
        if chosen is None:
            self.skipTest("no base-only key found")
        res = v.trace(chosen.resref, chosen.res_type)
        self.assertTrue(res.found)
        self.assertEqual(res.winner.pass_no, 5)
        self.assertEqual(res.winner.table.name, "chitin.key")


if __name__ == "__main__":
    unittest.main()
