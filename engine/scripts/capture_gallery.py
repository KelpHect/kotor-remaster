#!/usr/bin/env python3
"""Screenshot gallery: drives kotor_engine to produce one named PNG per shot.

Every shot is one engine run on one backend with a timeout: the engine draws the
requested screen for a fixed number of frames, writes a BMP (--screenshot), and this
tool converts it to PNG. Runs are strictly serial. The shots contain art from the
user's install: keep the output out of the repository (the default is the agent
scratchpad `screenshots` folder, or $KE_GALLERY_OUT, or ./gallery_out when neither
exists) and never upload it.

    capture_gallery.py --list
    capture_gallery.py --backend gl --out <dir> --only mainmenu,options
    capture_gallery.py --backend vulkan --exe <private copy of kotor_engine.exe>

The index (index.md and index.json in the output folder) lists each shot with its
backend, pass or fail, image size and the measured screen statistics. A pass here
means "the engine exited 0, wrote an image and the image is not blank"; it is NOT a
statement that the screen looks right. View the PNGs.

Other agents add shots by appending to SHOTS below (one line each).
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path

ENGINE_DIR = Path(__file__).resolve().parent.parent
DEFAULT_EXE = ENGINE_DIR / "build" / "msvc-release" / "kotor_engine.exe"
SCRATCH_OUT = Path(
    r"C:\dev-overflow\claude-tmp\claude\929b2355b375ed58\1737d0b6-d112-4c54-b781-d93fbfc74dc2\scratchpad\screenshots"
)

# A module is needed behind every screen until the front end runs without a session.
BASE_MODULE = "end_m01aa"


@dataclass
class Shot:
    name: str
    args: list[str]              # engine arguments after the backend/window ones
    frames: int = 24             # frames rendered before the screenshot
    size: tuple[int, int] = (1280, 720)
    group: str = "gui"
    timeout: int = 120
    note: str = ""
    extra: dict = field(default_factory=dict)


def gui(name: str, screen: str, *, actions: str = "", size=(1280, 720), frames: int = 24,
        group: str = "gui", note: str = "", more: list[str] | None = None) -> Shot:
    args = ["--module", BASE_MODULE, "--gui-screen", screen]
    if actions:
        args += ["--gui-action", actions]
    args += more or []
    return Shot(name, args, frames=frames, size=size, group=group, note=note)


def cin(name: str, args: list[str], *, frames: int = 24, size=(1280, 720), movies: bool = False,
        fixed_dt: float = 0.0, note: str = "") -> Shot:
    more = list(args)
    if fixed_dt > 0:
        more += ["--fixed-dt", str(fixed_dt)]
    if "--module" not in more:
        more = ["--module", BASE_MODULE] + more
    return Shot(name, more, frames=frames, size=size, group="cinematics", note=note,
                extra={"movies": True} if movies else {})


def sfx(name: str, vfx: int, *, frames: int = 45, note: str = "") -> Shot:
    """A visualeffects.2da row on the second-nearest creature, camera on it (--spawn-vfx)."""
    return Shot(name, ["--module", BASE_MODULE, "--spawn-vfx", f"{vfx}:1", "--capture-camera", "creature:1",
                       "--fixed-dt", "0.0333"], frames=frames, group="effects", note=note)


# --- the shot list ---------------------------------------------------------------------
# name, screen resref, optional --gui-action chain (comma separated tags, activated
# in order as real pointer clicks), window size.
SHOTS: list[Shot] = [
    gui("mainmenu", "mainmenu"),
    gui("mainmenu_800x600", "mainmenu", size=(800, 600)),
    gui("mainmenu_1920x1080", "mainmenu", size=(1920, 1080)),
    # Options, load/save, movies, credits: the shipped screens bound to real state.
    gui("optionsmain", "optionsmain", group="frontend"),
    gui("optgameplay", "optgameplay", group="frontend"),
    gui("optautopause", "optautopause", group="frontend"),
    gui("optgraphics", "optgraphics", group="frontend"),
    gui("optsound", "optsound", group="frontend"),
    gui("optfeedback", "optfeedback", group="frontend"),
    gui("optionsingame", "optionsingame", group="frontend"),
    gui("saveload", "saveload", group="frontend"),
    gui("titlemovie", "titlemovie", group="frontend"),
    gui("credits", "credits", group="frontend", frames=140),
    # In-game: the HUD over the world, and the shipped screens a key opens (--panel).
    Shot("hud", ["--module", BASE_MODULE, "--fixed-dt", "0.05"], frames=1200, group="ingame", timeout=240,
         note="the module fades in from black first"),
    Shot("inventory", ["--module", BASE_MODULE, "--panel", "inventory"], frames=30, group="ingame"),
    Shot("journal", ["--module", BASE_MODULE, "--panel", "journal"], frames=30, group="ingame"),
    Shot("character", ["--module", BASE_MODULE, "--panel", "character"], frames=30, group="ingame"),
    # Cinematics (movies keep their audio off; --fixed-dt makes the moment reproducible).
    cin("cin_movie_leclogo", ["--movie-play", "leclogo", "--movie-time", "2.0"], movies=True),
    cin("cin_movie_biologo", ["--movie-play", "biologo", "--movie-time", "2.0"], movies=True),
    cin("cin_movie_legal", ["--movie-play", "legal", "--movie-time", "2.0"], movies=True),
    cin("cin_movie_01a", ["--movie-play", "01a", "--movie-time", "20.0"], movies=True),
    cin("cin_movie_55", ["--movie-play", "55", "--movie-time", "10.0"], movies=True),
    cin("cin_stunt55a", ["--module", "STUNT_55a"], frames=60, fixed_dt=0.05),
    cin("cin_dialogue_trask", ["--demo-dialogue"], frames=12, fixed_dt=0.1,
        note="shipped conversation with a voice-over: lip rig driven by the VO's LIP file"),
    cin("cin_dialogue_trask_t6", ["--demo-dialogue"], frames=70, fixed_dt=0.1),
    cin("cin_stunt55a_t8", ["--module", "STUNT_55a"], frames=160, fixed_dt=0.05,
        note="same cutscene, 8 s in: the conversation advances by itself on the voice-over"),
    # Effects: visualeffects.2da rows held on a creature (game/Effects.cpp).
    sfx("fx_heal_1001", 1001, note="VFX_IMP_HEALING_SMALL v_heal_imp"),
    sfx("fx_force_armor_1010", 1010, note="VFX_PRO_FORCE_ARMOR v_farmor_imp"),
    sfx("fx_whirlwind_2007", 2007, note="VFX_DUR_FORCE_WHIRLWIND v_whwind_dur"),
    sfx("fx_shield_red_2035", 2035, note="VFX_DUR_SHIELD_RED_MARK_IV v_fieldmrk3_dur"),
    sfx("fx_gravity_3018", 3018, note="VFX_FNF_GRAVITY_GENERATOR_2 v_gravgen2_fnf"),
    sfx("fx_explosion_4035", 4035, frames=14, note="VFX_COM_DROID_EXPLOSION_2 v_drdexpld_02"),
]

# The 3D world: the module entry view and a close look at a creature, UI hidden.
WORLD_MODULES = [
    "end_m01aa", "tar_m02aa", "tar_m03aa", "danm13", "kas_m25aa", "tat_m17aa", "korr_m34aa",
    "unk_m41aa", "lev_m40aa", "sta_m45aa", "ebo_m41aa",
    "STUNT_00", "STUNT_03a", "STUNT_06", "STUNT_07", "STUNT_12", "STUNT_14", "STUNT_16", "STUNT_18",
    "STUNT_19", "STUNT_31b", "STUNT_34", "STUNT_35", "STUNT_42", "STUNT_44", "STUNT_50a", "STUNT_51a",
    "STUNT_54a", "STUNT_55a", "STUNT_56a", "STUNT_57",
]
# A fixed clock, so one shot shows the same moment on every backend and they can be compared.
_WORLD_CLOCK = ["--fixed-dt", "0.0166667"]
for _module in WORLD_MODULES:
    for _view in ("entry", "creature:1"):
        SHOTS.append(Shot(f"world_{_module.lower()}_{_view.split(':')[0]}",
                          ["--module", _module, "--capture-camera", _view] + _WORLD_CLOCK, frames=90, group="world"))
# What only shows once something happens: a live fire, a door swinging, an equip change, a sky
# room, and a cutscene's own camera (its camerahook, with VIS culling from where that camera is).
for _name, _args, _frames in [
    ("world_fire_closeup", ["--module", "tar_m04aa", "--capture-camera", "emitter:smfire:0"], 150),
    ("world_door_closed", ["--module", "end_m01aa", "--capture-camera", "door:0"], 60),
    ("world_door_swinging", ["--module", "end_m01aa", "--capture-camera", "door:0",
                             "--capture-action", "door:0@50"], 110),
    ("world_door_open", ["--module", "end_m01aa", "--capture-camera", "door:0",
                         "--capture-action", "door:0@30"], 150),
    ("world_equip_before", ["--module", "end_m01aa", "--capture-camera", "creature:2"], 60),
    ("world_equip_after", ["--module", "end_m01aa", "--capture-camera", "creature:2",
                           "--capture-action", "equip:2:w_vbroswrd_001@30"], 60),
    ("world_sky_taris", ["--module", "tar_m02aa", "--capture-camera", "112.5,112.5,3,112.5,130,12"], 60),
    ("world_cutscene_camera_57", ["--module", "STUNT_57"], 240),
    ("world_cutscene_camera_34", ["--module", "STUNT_34"], 240),
]:
    SHOTS.append(Shot(_name, _args + _WORLD_CLOCK, frames=_frames, group="world"))

# A talking close-up: a shipped line over time, then single mouth shapes held still.
_TALK_VIEW = ["--demo-dialogue", "--capture-camera", "node:end_trask:headhook:0.5"]
for _frames in (6, 12, 18, 24, 30, 36):
    SHOTS.append(cin(f"cin_talk_trask_f{_frames:02d}", _TALK_VIEW, frames=_frames, fixed_dt=0.1))
for _shape, _label in ((0, "silence"), (3, "ox"), (11, "bump"), (13, "roar")):
    SHOTS.append(cin(f"cin_talk_shape_{_label}", _TALK_VIEW + ["--capture-action", f"lip:{_shape}@5"],
                     frames=16, fixed_dt=0.05))
# What the hands hold: the PC and a companion, standing, ready, walking and swinging, through the
# real equip, on the rhand and lhand hooks.
_WIELDS = {
    "melee": "g_w_lngswrd01",
    "twohanded": "g_w_qtrstaff01",
    "saber": "g_w_lghtsbr01",
    "blaster": "g_w_blstrpstl001",
    "rifle": "g_w_blstrrfl001",
}
for _who, _tag, _setup in (("pc", "PC", []), ("companion", "Carth", ["join:Carth@2"])):
    for _weapon, _item in _WIELDS.items():
        _base = _setup + [f"wield:{_tag}:{_item}@4"]
        for _pose, _more, _count in (
            ("idle", [], 30),
            ("ready", [f"combat:{_tag}:1@6"], 40),
            ("walk", [f"combat:{_tag}:1@6", f"stance:{_tag}:walk@30"], 42),
            ("attack", [f"combat:{_tag}:1@6", f"attack:{_tag}@30"], 38),
        ):
            SHOTS.append(Shot(f"world_wield_{_who}_{_weapon}_{_pose}",
                              ["--module", "end_m01aa", "--third-person", "--fixed-dt", "0.033",
                               "--capture-camera", f"node:{_tag}:torso_g:2.2",
                               "--capture-action", ",".join(_base + _more)],
                              frames=_count, group="world"))


def lookup(shots: list[Shot], names: str | None) -> list[Shot]:
    if not names:
        return shots
    wanted = [n.strip() for n in names.split(",") if n.strip()]
    chosen = []
    for want in wanted:
        hits = [s for s in shots if s.name == want or s.group == want
                or (want.endswith("*") and s.name.startswith(want[:-1]))]
        if not hits:
            raise SystemExit(f"no shot or group named '{want}' (use --list)")
        chosen += [h for h in hits if h not in chosen]
    return chosen


# --- BMP -> PNG, no dependencies -----------------------------------------------------------
def read_bmp(path: Path) -> tuple[int, int, bytes]:
    """Returns width, height, tightly packed RGB rows top to bottom."""
    d = path.read_bytes()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    step = bpp // 8
    pitch = (w * step + 3) & ~3
    top_down = h < 0
    h = abs(h)
    out = bytearray()
    for y in range(h):
        row = y if top_down else h - 1 - y
        start = off + row * pitch
        px = d[start:start + w * step]
        if step == 3 or step == 4:
            rgb = bytearray(w * 3)
            rgb[0::3] = px[2::step][:w]
            rgb[1::3] = px[1::step][:w]
            rgb[2::3] = px[0::step][:w]
            out += rgb
        else:
            raise ValueError(f"unsupported BMP depth {bpp}")
    return w, h, bytes(out)


def write_png(path: Path, w: int, h: int, rgb: bytes) -> None:
    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    raw = bytearray()
    for y in range(h):
        raw += b"\0" + rgb[y * w * 3:(y + 1) * w * 3]
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def image_stats(w: int, h: int, rgb: bytes) -> dict:
    """Cheap blank/garbage detection: distinct colours (sampled) and mean brightness."""
    colours = set()
    total = 0
    step = max(1, (w * h) // 40000)
    for i in range(0, w * h, step):
        p = rgb[i * 3:i * 3 + 3]
        colours.add((p[0] >> 2, p[1] >> 2, p[2] >> 2))
        total += p[0] + p[1] + p[2]
    n = len(range(0, w * h, step))
    return {"colours": len(colours), "mean": round(total / (3 * max(n, 1)), 1)}


# --- running ------------------------------------------------------------------------------
BACKEND_FLAG = {"gl": "--gl", "opengl": "--gl", "vulkan": "--vulkan", "vk": "--vulkan"}


def run_shot(shot: Shot, backend: str, exe: Path, out_dir: Path, install: str | None,
             log_dir: Path, hidden: bool) -> dict:
    bmp = out_dir / f"{shot.name}.bmp"
    png = out_dir / f"{shot.name}.png"
    bmp.unlink(missing_ok=True)
    png.unlink(missing_ok=True)
    cmd = [str(exe), BACKEND_FLAG[backend], "--no-fallback", "--width", str(shot.size[0]),
           "--height", str(shot.size[1]), "--frames", str(shot.frames),
           "--screenshot", str(bmp), "--no-audio"]
    if shot.group == "frontend" or shot.name.startswith("mainmenu"):
        # An isolated saves folder (one real save, written by the engine) and options file.
        cmd += ["--saves-dir", str(out_dir.parent / "saves"),
                "--settings-file", str(out_dir.parent / f"settings_{backend}.ini")]
    if not shot.extra.get("movies"):  # a movie shot sets extra={"movies": True}
        cmd.append("--no-movies")
    if hidden:
        cmd.append("--hidden")
    if install:
        cmd += ["--install", install]
    cmd += shot.args
    log = log_dir / f"{backend}_{shot.name}.log"
    start = time.time()
    record = {"name": shot.name, "group": shot.group, "backend": backend, "ok": False,
              "size": None, "seconds": 0.0, "detail": "", "note": shot.note,
              "cmd": " ".join(cmd[1:])}
    try:
        with open(log, "wb") as handle:
            proc = subprocess.run(cmd, stdout=handle, stderr=subprocess.STDOUT,
                                  timeout=shot.timeout, stdin=subprocess.DEVNULL)
        code = proc.returncode
    except subprocess.TimeoutExpired:
        record["detail"] = f"timeout after {shot.timeout}s"
        record["seconds"] = round(time.time() - start, 1)
        return record
    record["seconds"] = round(time.time() - start, 1)
    if code != 0:
        record["detail"] = f"exit code {code} (see {log.name})"
        return record
    if not bmp.exists():
        record["detail"] = "no screenshot written"
        return record
    try:
        w, h, rgb = read_bmp(bmp)
        write_png(png, w, h, rgb)
        bmp.unlink(missing_ok=True)
    except Exception as error:  # noqa: BLE001 - report any decode problem as a failure
        record["detail"] = f"bad image: {error}"
        return record
    stats = image_stats(w, h, rgb)
    record["size"] = f"{w}x{h}"
    record["stats"] = stats
    if stats["colours"] < 8:
        record["detail"] = f"blank or near-uniform frame ({stats['colours']} colours)"
        return record
    record["ok"] = True
    return record


def write_index(out_dir: Path, records: list[dict]) -> None:
    (out_dir / "index.json").write_text(json.dumps(records, indent=1), encoding="utf-8")
    lines = ["# Gallery index", "",
             "pass = engine exited 0, wrote an image, image not blank. It does not mean the screen is right.",
             "", "| shot | backend | result | size | colours | seconds | detail |",
             "|---|---|---|---|---|---|---|"]
    for r in records:
        stats = r.get("stats") or {}
        lines.append(f"| {r['name']} | {r['backend']} | {'PASS' if r['ok'] else 'FAIL'} | "
                     f"{r['size'] or '-'} | {stats.get('colours', '-')} | {r['seconds']} | "
                     f"{r['detail'] or r['note']} |")
    passed = sum(1 for r in records if r["ok"])
    lines += ["", f"{passed}/{len(records)} passed"]
    (out_dir / "index.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def ensure_saves(exe: Path, out_root: Path, install: str | None) -> None:
    """One real save for the load screen: the engine writes it (--original-save-create)."""
    saves = out_root / "saves"
    target = saves / "000001 - Gallery"
    if (target / "savenfo.res").exists():
        return
    saves.mkdir(parents=True, exist_ok=True)
    cmd = [str(exe), "--original-save-create", str(target), "--null", "--headless", "--no-audio"]
    if install:
        cmd += ["--install", install]
    try:
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, timeout=180,
                       stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        print("save fixture: timeout", file=sys.stderr)


def default_out() -> Path:
    env = os.environ.get("KE_GALLERY_OUT")
    if env:
        return Path(env)
    if SCRATCH_OUT.parent.exists():
        return SCRATCH_OUT
    return Path.cwd() / "gallery_out"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="list shots and exit")
    parser.add_argument("--only", help="comma list of shot names, group names or prefix*")
    parser.add_argument("--backend", default="gl", choices=sorted(BACKEND_FLAG), help="gl or vulkan")
    parser.add_argument("--exe", default=str(DEFAULT_EXE), help="engine binary (a private copy avoids link locks)")
    parser.add_argument("--out", default=None, help="output folder (default: scratchpad screenshots)")
    parser.add_argument("--install", default=None, help="KotOR install folder")
    parser.add_argument("--no-hidden", action="store_true", help="show the window while capturing")
    args = parser.parse_args()

    shots = lookup(SHOTS, args.only)
    if args.list:
        for s in shots:
            print(f"{s.name:28} {s.group:10} {s.size[0]}x{s.size[1]:<5} {' '.join(s.args)}")
        return 0
    exe = Path(args.exe)
    if not exe.exists():
        print(f"engine binary not found: {exe}", file=sys.stderr)
        return 2
    out_root = Path(args.out) if args.out else default_out()
    backend_name = "vulkan" if args.backend in ("vulkan", "vk") else "gl"
    out_dir = out_root / backend_name
    log_dir = out_root / "logs"
    out_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)

    ensure_saves(exe, out_root, args.install)
    records = []
    for shot in shots:
        print(f"[{backend_name}] {shot.name} ...", flush=True)
        record = run_shot(shot, args.backend, exe, out_dir, args.install, log_dir, not args.no_hidden)
        print(f"    {'PASS' if record['ok'] else 'FAIL'} {record['size'] or ''} {record['detail']}", flush=True)
        records.append(record)
    write_index(out_dir, records)
    failed = [r for r in records if not r["ok"]]
    print(f"{len(records) - len(failed)}/{len(records)} passed; index: {out_dir / 'index.md'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
