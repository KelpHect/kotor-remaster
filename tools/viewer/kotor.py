"""Read-only access to the user's KotOR install for the s1-08 viewer.

Wraps ``tools/resources/kotor_archive.py`` (imported read-only) with a small
VFS-lite: module RIMs (``modules/<stem>.rim``, ``<stem>_s.rim``), ``patch.erf``,
the texture-pack ERFs (``TexturePacks/swpc_tex_*.erf``) and the base
``chitin.key``/BIF index.  Lookup order follows the documented five-pass VFS
(docs/recon/08_VFS_DYNAMIC.md) restricted to the sources this lane needs:
module RIMs -> patch.erf -> texture packs (gui, then quality tier tpa) ->
base KEY/BIFs.  The install is only ever opened ``rb``; a read-only hash
sentinel check on ``swkotor.exe`` is provided and stops on drift.
"""

from __future__ import annotations

import hashlib
import os

from . import BUILD_SHA256, game_dir, load_module

ka = load_module("s1_08_kotor_archive", "tools/resources/kotor_archive.py")

# resource type ids (tools/inventory/kotor_types.py, docs/recon/01)
T_MDL, T_TGA, T_WOK, T_MDX = 2002, 3, 2016, 3008
T_ARE, T_IFO, T_GIT, T_GUI, T_LYT, T_VIS, T_PTH, T_TPC, T_TXI = (
    2012, 2014, 2023, 2047, 3000, 3001, 3003, 3007, 2022)


class InstallSentinelError(RuntimeError):
    pass


def exe_sha256(gdir: str) -> str:
    h = hashlib.sha256()
    with open(os.path.join(gdir, "swkotor.exe"), "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check_install_sentinel(gdir: str) -> dict:
    """Read-only hash sentinel; raises InstallSentinelError on drift."""
    digest = exe_sha256(gdir)
    ok = digest == BUILD_SHA256
    if not ok:
        raise InstallSentinelError(
            f"swkotor.exe SHA-256 {digest} != expected {BUILD_SHA256}; "
            "install drift, stopping (read-only lane)")
    return {"exe": "swkotor.exe", "sha256": digest, "matches_build": True}


class Game:
    """Read-only resource lookup over one install."""

    def __init__(self, gdir: str | None = None):
        self.dir = game_dir(gdir)
        self._key = None
        self._bifs: dict[int, object] = {}
        self._rims: dict[str, object] = {}
        self._erfs: dict[str, object] = {}
        self.module_stem: str | None = None
        self.sources: list[tuple[str, object]] = []

    # -- lazy archives ----------------------------------------------------
    @property
    def key(self):
        if self._key is None:
            self._key = ka.read_key(os.path.join(self.dir, "chitin.key"))
        return self._key

    def bif(self, bif_index: int):
        if bif_index not in self._bifs:
            entry = self.key.bifs[bif_index]
            path = ka.bif_path_for(self.dir, entry["name"])
            self._bifs[bif_index] = ka.read_bif(path)
        return self._bifs[bif_index]

    def rim(self, stem: str):
        if stem not in self._rims:
            path = os.path.join(self.dir, "modules", stem + ".rim")
            self._rims[stem] = ka.read_rim(path) if os.path.exists(path) else None
        return self._rims[stem]

    def erf(self, relpath: str):
        if relpath not in self._erfs:
            path = os.path.join(self.dir, relpath)
            self._erfs[relpath] = ka.read_erf(path) if os.path.exists(path) else None
        return self._erfs[relpath]

    def use_module(self, stem: str):
        """Bind module RIMs (<stem>.rim and <stem>_s.rim) as first lookup tier."""
        self.module_stem = stem
        self.sources = []
        for s in (stem, stem + "_s"):
            r = self.rim(s)
            if r is not None:
                self.sources.append((f"modules/{s}.rim", r))

    def add_packs(self):
        for rel in ("patch.erf", "TexturePacks/swpc_tex_gui.erf",
                    "TexturePacks/swpc_tex_tpa.erf"):
            a = self.erf(rel)
            if a is not None:
                self.sources.append((rel, a))

    # -- lookup -----------------------------------------------------------
    def find(self, resref: str, res_type: int) -> tuple[bytes, str] | None:
        """First hit across bound sources, then base KEY/BIFs."""
        for src, arch in self.sources:
            e = arch.lookup(resref, res_type)
            if e is not None:
                return arch.read_entry(e), src
        ke = self.key.lookup(resref, res_type)
        if ke is not None:
            rec = self.bif(ke.bif_index).by_id[ke.resid]
            return self.bif(ke.bif_index).read_record(rec), \
                f"{self.key.bifs[ke.bif_index]['name']} (KEY)"
        return None

    def find_texture(self, resref: str) -> tuple[bytes, int, str] | None:
        """Texture lookup preferring TPC (packs) over base TGA."""
        hit = self.find(resref, T_TPC)
        if hit:
            return hit[0], T_TPC, hit[1]
        hit = self.find(resref, T_TGA)
        if hit:
            return hit[0], T_TGA, hit[1]
        return None
