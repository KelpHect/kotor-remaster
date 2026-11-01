"""Lane s1-08: tools-side textured geometry + GUI viewer reading original data.

Self-contained pure-Python (stdlib only) viewer package.  It reads the user's
KotOR install strictly read-only through ``KOTOR_GAME_DIR`` and imports the
shared project parsers read-only:

* ``tools/formats/gff.py``            (GFF V3.2 reader)
* ``tools/resources/kotor_archive.py`` (KEY/BIF/RIM/ERF containers)

It owns its own minimal LYT / WOK(BWM) / MDL+MDX / TPC / TGA / GUI decoders
(modules ``lyt``, ``wok``, ``mdl``, ``tpc``, ``gui``) and never edits
``tools/formats`` or ``tools/geometry``.  Output is pure-Python PNG
(zlib+struct) via ``pngwrite`` plus JSON scene summaries.  All bulk outputs go
to ``artifacts/lanes/s1-08-viewer/`` (gitignored); nothing game-derived is
written to tracked files.

Build under test: SHA-256
``0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6``.
"""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

BUILD_SHA256 = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6"

DEFAULT_GAME_DIR = "E:/SteamLibrary/steamapps/common/swkotor"


def load_module(name: str, relpath: str):
    """Import a sibling project module by file path (read-only)."""
    path = REPO_ROOT / relpath
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def game_dir(env: str | None = None) -> str:
    """Resolve the read-only game dir from KOTOR_GAME_DIR.

    Accepts Windows paths (``E:/...``) and Git Bash paths (``/e/...``); the
    latter are rewritten to drive-letter form for the native interpreter.
    """
    raw = env if env is not None else os.environ.get("KOTOR_GAME_DIR", "")
    if not raw:
        raw = DEFAULT_GAME_DIR
    raw = raw.replace("\\", "/")
    if len(raw) > 2 and raw[0] == "/" and raw[1].isalpha() and raw[2] == "/":
        raw = raw[1].upper() + ":/" + raw[3:]
    return raw
