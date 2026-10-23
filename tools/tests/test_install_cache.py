#!/usr/bin/env python3
"""Tests for the read-only install validator and external cache profile.

Layout assumption (proposed integration): this file lives in
``tools/tests/`` and the modules under test live in ``tools/install/``.

Run::

    python -m unittest -v test_install_cache            # from tools/tests
    python tools/tests/test_install_cache.py            # direct

The real-install test uses ``KOTOR_GAME_DIR`` or ``KOTOR_INSTALL`` and is
skipped when neither points at an install. All other tests build a synthetic
key/BIF/TLK/RIM/ERF fixture in a temporary directory; no game data is copied.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INSTALL_DIR = os.path.abspath(os.path.join(HERE, "..", "install"))
if INSTALL_DIR not in sys.path:
    sys.path.insert(0, INSTALL_DIR)

import cache_profile  # noqa: E402
import install_validate  # noqa: E402

REAL_DIR = os.environ.get("KOTOR_GAME_DIR") or os.environ.get("KOTOR_INSTALL")


# --------------------------------------------------------------------------
# Synthetic fixture builders
# --------------------------------------------------------------------------
def build_key(path: str, bifs: list[tuple[str, int]], entries: list[tuple[str, int, int]]) -> None:
    """bifs: (name, declared_size); entries: (resref, type, resid)."""
    off_files = 64
    bif_count = len(bifs)
    name_off = off_files + bif_count * 12
    names = b""
    records = []
    for name, size in bifs:
        raw = name.encode("latin-1")
        records.append(struct.pack("<IIHH", size, name_off + len(names), len(raw), 1))
        names += raw
    off_keys = name_off + len(names)
    header = b"KEY V1  " + struct.pack("<4I", bif_count, len(entries), off_files, off_keys)
    header = header.ljust(off_files, b"\x00")
    body = b"".join(records) + names
    key_entries = b""
    for resref, rtype, resid in entries:
        key_entries += resref.encode("latin-1").ljust(16, b"\x00")
        key_entries += struct.pack("<HI", rtype, resid)
    with open(path, "wb") as f:
        f.write(header + body + key_entries)


def build_bif(path: str, resources: list[tuple[int, int, bytes]]) -> int:
    """resources: (id, type, payload). Layout: header, var table, fixed table, data."""
    fixed_count = len(resources)
    off_var = 20
    off_fixed = off_var  # var_count == 0
    payload_off = off_fixed + fixed_count * 16
    table = b""
    blob = b""
    for rid, rtype, payload in resources:
        table += struct.pack("<4I", rid, payload_off + len(blob), len(payload), rtype)
        blob += payload
    data = b"BIFFV1  " + struct.pack("<3I", 0, fixed_count, off_var) + table + blob
    with open(path, "wb") as f:
        f.write(data)
    return len(data)


def build_tlk(path: str, language: int = 0, count: int = 1) -> None:
    entries_offset = 20 + count * 40
    header = b"TLK V3.0" + struct.pack("<3I", language, count, entries_offset)
    entries = b"".join(
        struct.pack("<I", 1) + b"\x00" * 16 + struct.pack("<5I", 0, 0, 0, 0, 0)
        for _ in range(count)
    )
    with open(path, "wb") as f:
        f.write(header + entries)


def build_erf(path: str, count: int = 1) -> None:
    off_key = 32
    off_res = off_key + count * 24
    size = off_res + count * 8
    head = b"ERF V1.0" + b"\x00" * 8 + struct.pack("<I", count) + struct.pack("<I", off_key)
    head += b"\x00" * 4 + struct.pack("<I", off_res) + b"\x00" * (32 - len(head))
    keys = (b"mini".ljust(16, b"\x00") + struct.pack("<IHH", 0, 2017, 0)) * count
    res = struct.pack("<II", 0, 0) * count
    with open(path, "wb") as f:
        f.write(head + keys + res)


def build_rim(path: str) -> None:
    with open(path, "wb") as f:
        f.write(b"RIM V1.0" + b"\x00" * 4 + struct.pack("<2I", 0, 0))


def make_install(root: str, *, with_tlk: bool = True, with_modules: bool = True) -> str:
    os.makedirs(os.path.join(root, "data"), exist_ok=True)
    os.makedirs(os.path.join(root, "modules"), exist_ok=True)
    os.makedirs(os.path.join(root, "rims"), exist_ok=True)
    bif_size = build_bif(os.path.join(root, "data", "mini.bif"),
                         [(0, 2017, b"2DA V2.0\n"), (1, 2010, b"NCS\x00")])
    build_key(os.path.join(root, "chitin.key"), [("data\\mini.bif", bif_size)],
              [("mini", 2017, 0), ("mini", 2010, 1)])
    if with_tlk:
        build_tlk(os.path.join(root, "dialog.tlk"))
    build_erf(os.path.join(root, "patch.erf"), count=1)
    if with_modules:
        build_rim(os.path.join(root, "modules", "m01aa.rim"))
    build_rim(os.path.join(root, "rims", "global.rim"))
    with open(os.path.join(root, "README.txt"), "w", encoding="ascii") as f:
        f.write("fixture\n")
    return root


class TempCase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="kotor-install-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.install = os.path.join(self.tmp, "install")
        os.makedirs(self.install, exist_ok=True)


# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------
class DetectTests(TempCase):
    def test_detect_from_install_root_and_subdirectory(self) -> None:
        make_install(self.install)
        self.assertEqual(install_validate.detect_install(self.install)["root"],
                         os.path.abspath(self.install))
        self.assertEqual(install_validate.detect_install(os.path.join(self.install, "data"))["root"],
                         os.path.abspath(self.install))

    def test_detect_returns_none_for_empty_dir(self) -> None:
        self.assertIsNone(install_validate.detect_install(self.tmp)["root"])


class SyntheticValidationTests(TempCase):
    def test_valid_synthetic_install(self) -> None:
        make_install(self.install)
        report = install_validate.validate_install(self.install)
        self.assertTrue(report["ok"], report["issues"])
        self.assertEqual(report["key"]["key_count"], 2)
        self.assertEqual(report["key"]["bif_count"], 1)
        self.assertEqual(report["bifs"][0]["table_resources"], 2)
        self.assertEqual(report["bifs"][0]["key_entries"], 2)
        self.assertEqual(report["tlk"]["string_count"], 1)
        self.assertEqual(report["tlk"]["language"], "english")
        self.assertFalse(report["dataset"]["known_reference"])
        self.assertIsNone(report["dataset"]["product_version"])
        self.assertEqual(report["archives"]["modules"]["count"], 1)
        self.assertEqual(report["archives"]["rims"]["count"], 1)
        self.assertEqual(report["archives"]["patch_erf"]["entry_count"], 1)

    def test_missing_required_file(self) -> None:
        make_install(self.install)
        os.remove(os.path.join(self.install, "dialog.tlk"))
        report = install_validate.validate_install(self.install)
        self.assertFalse(report["ok"])
        codes = {(i["code"], i.get("path")) for i in report["issues"]}
        self.assertIn(("missing_required_file", "dialog.tlk"), codes)

    def test_missing_bif_referenced_by_key(self) -> None:
        make_install(self.install)
        os.remove(os.path.join(self.install, "data", "mini.bif"))
        report = install_validate.validate_install(self.install)
        self.assertFalse(report["ok"])
        self.assertIn("bif_missing", {i["code"] for i in report["issues"]})

    def test_wrong_hash_via_expectation(self) -> None:
        make_install(self.install)
        bad = "0" * 64
        report = install_validate.validate_install(self.install, expect={"dialog.tlk": bad})
        self.assertFalse(report["ok"])
        row = [i for i in report["issues"] if i["code"] == "hash_mismatch"]
        self.assertEqual(len(row), 1)
        self.assertEqual(row[0]["path"], "dialog.tlk")

    def test_missing_required_directory(self) -> None:
        make_install(self.install)
        shutil.rmtree(os.path.join(self.install, "rims"))
        report = install_validate.validate_install(self.install)
        self.assertFalse(report["ok"])
        self.assertIn(("missing_required_dir", "rims/"),
                      {(i["code"], i.get("path")) for i in report["issues"]})


class ProfileTests(TempCase):
    def _build(self, name: str) -> tuple[dict, str]:
        out = os.path.join(self.tmp, "cache", name)
        profile = cache_profile.build_profile(self.install, scope="dataset")
        path = cache_profile.write_profile(profile, out, install_root=self.install)
        return profile, path

    def test_profile_is_deterministic_and_verifies(self) -> None:
        make_install(self.install)
        p1, path1 = self._build("p1.json")
        p2, path2 = self._build("p2.json")
        self.assertEqual(p1["profile_hash"], p2["profile_hash"])
        with open(path1, "rb") as f1, open(path2, "rb") as f2:
            self.assertEqual(f1.read(), f2.read())
        self.assertEqual(p1["counts"]["files"], 6)  # key, tlk, patch, bif, module rim, global rim
        self.assertEqual(p1["counts"]["by_kind"],
                         {"data_bif": 1, "key": 1, "module_rim": 1, "patch_erf": 1, "rim": 1, "talk_table": 1})
        result = cache_profile.verify_profile(path1)
        self.assertTrue(result["ok"], result["issues"])
        self.assertTrue(result["profile_hash_match"])
        self.assertEqual(result["files_matched"], result["files_recorded"])

    def test_verify_detects_wrong_hash_same_size(self) -> None:
        make_install(self.install)
        _profile, path = self._build("p.json")
        target = os.path.join(self.install, "dialog.tlk")
        with open(target, "r+b") as f:
            f.seek(20)  # first flags word; keep size identical
            f.write(b"\x02\x00\x00\x00")
        result = cache_profile.verify_profile(path)
        self.assertFalse(result["ok"])
        self.assertIn("hash_mismatch", {i["code"] for i in result["issues"]})

    def test_verify_detects_missing_file_and_size_change(self) -> None:
        make_install(self.install)
        _profile, path = self._build("p.json")
        os.remove(os.path.join(self.install, "data", "mini.bif"))
        result = cache_profile.verify_profile(path)
        self.assertFalse(result["ok"])
        self.assertIn("missing", {i["code"] for i in result["issues"]})

        # Rebuild, then change a size without touching any hash.
        make_install(self.install)
        _profile, path = self._build("p2.json")
        with open(os.path.join(self.install, "dialog.tlk"), "ab") as f:
            f.write(b"\x00")
        result = cache_profile.verify_profile(path)
        self.assertFalse(result["ok"])
        self.assertIn("size_mismatch", {i["code"] for i in result["issues"]})

    def test_refuses_output_inside_install(self) -> None:
        make_install(self.install)
        profile = cache_profile.build_profile(self.install, scope="dataset")
        with self.assertRaises(cache_profile.CacheOutputRefused):
            cache_profile.write_profile(profile, os.path.join(self.install, "cache.json"))
        nested = os.path.join(self.install, "saves", "cache")
        with self.assertRaises(cache_profile.CacheOutputRefused):
            cache_profile.assert_output_allowed(os.path.join(nested, "p.json"))

    def test_refuses_output_in_dir_containing_swkotor_exe(self) -> None:
        decoy = os.path.join(self.tmp, "other")
        os.makedirs(decoy)
        open(os.path.join(decoy, "swkotor.exe"), "wb").close()
        with self.assertRaises(cache_profile.CacheOutputRefused):
            cache_profile.assert_output_allowed(os.path.join(decoy, "profile.json"))

    def test_profile_tamper_changes_hash(self) -> None:
        make_install(self.install)
        profile, path = self._build("p.json")
        profile["files"][0]["sha256"] = "f" * 64
        self.assertNotEqual(cache_profile.profile_digest(profile), profile["profile_hash"])
        result = cache_profile.verify_profile(path)  # file on disk is untouched
        self.assertTrue(result["ok"])


@unittest.skipUnless(REAL_DIR and os.path.isdir(REAL_DIR), "KOTOR_GAME_DIR/KOTOR_INSTALL not set")
class RealInstallTests(unittest.TestCase):
    def test_real_install_validates_without_executable_dependency(self) -> None:
        report = install_validate.validate_install(REAL_DIR)
        self.assertTrue(report["ok"], [i for i in report["issues"] if i["severity"] == "error"])
        self.assertEqual(report["key"]["key_count"], 25836)
        self.assertEqual(report["key"]["bif_count"], 26)
        self.assertEqual(report["tlk"]["string_count"], 49265)
        self.assertEqual(report["tlk"]["language"], "english")
        self.assertEqual(report["dataset"]["product_version"], "1.0.3.0")
        self.assertTrue(report["dataset"]["known_reference"])
        self.assertEqual(report["archives"]["modules"]["count"], 234)
        self.assertEqual(report["archives"]["rims"]["count"], 12)
        self.assertFalse(any(i["severity"] == "error" for i in report["issues"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
