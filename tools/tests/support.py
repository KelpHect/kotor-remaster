"""Shared paths/imports for lane 05 tests."""
from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

LANE_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS = REPO_ROOT / "tools" / "resources"
FIXTURES = REPO_ROOT / "artifacts" / "tests" / "fixtures"
SAVE_FIXTURES = FIXTURES / "save"
VFS_FIXTURES = FIXTURES / "vfs"
VFS_GAME = VFS_FIXTURES / "game"
TMP = REPO_ROOT / "artifacts" / "tests" / "tmp"
HARNESS = TOOLS
TMP.mkdir(parents=True, exist_ok=True)
REAL_GAME = Path(os.environ.get("KOTOR_GAME_DIR") or "<not-set>")
REAL_SAVES = REAL_GAME / "saves"

for _p in (TOOLS, HARNESS, REPO_ROOT / "tools" / "formats",
           REPO_ROOT / "tools" / "inventory"):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))


def ensure_fixtures() -> None:
    if not (VFS_GAME / "chitin.key").exists() or not (SAVE_FIXTURES / "000002 - SYNTHETIC SAVE").exists():
        import build_fixtures
        build_fixtures.build_all()


def clean_tmp(name: str) -> Path:
    path = TMP / name
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)
    return path


def real_save_folders() -> list[Path]:
    if not REAL_SAVES.is_dir():
        return []
    out = []
    for p in sorted(REAL_SAVES.iterdir(), key=lambda x: x.name.lower()):
        if p.is_dir() and any(c.name.lower() == "savegame.sav" for c in p.iterdir()):
            out.append(p)
    return out
