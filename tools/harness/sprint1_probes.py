#!/usr/bin/env python3
"""Lane s1-06-dynamic (Sprint 1) probe harness: static preparation + consent-gated run plan.

The game is NEVER launched by this script.  ``run`` refuses unless the integrator has
placed a consent file (``artifacts/lanes/s1-06-dynamic/CONSENT``) AND passed
``--consent``; the live session is driven separately through
``tools.mcp_hydralysis.runtime`` from ``run_typescript`` using the generated
``probe_plan.json``.  See docs/recon/22_DYNAMIC_P1.md.

Subcommands (all read-only against the install):

  preflight   read-only hash sentinel on the install (stop on drift), single-instance
              check, backend note; writes artifacts/lanes/s1-06-dynamic/preflight.json
  profile     build/verify the profile-local working directory: patched-copy
              swkotor.exe (windowed 1280x720 mod on a COPY, never the install),
              swkotor.ini (FullScreen=0 ...), junctions into the read-only install,
              writable profile-local saves/Override/logs.  Fresh save slots only.
  manifest    build static probe manifests via tools/resources/build_dynamic_probes.py
              plus a streamwaves (resref, WAV/4) sample manifest
  plan        write probe_plan.json: measurements (a)-(d) with hook addresses and the
              build SHA-256, and the consent gate status
  compare     compare a captured hook log against a manifest via
              tools/resources/vfs_measure.py
  run         consent gate only; never launches the game itself

Safety rules implemented here: never write inside the install; stop on sentinel drift;
refuse to run while another swkotor.exe instance exists (one owned instance at a time);
the install executable is never patched (the windowed mod is applied to a profile copy
and hash-checked); original saves are never overwritten (profile-local saves dir only).

Usage (Git Bash):
  python tools/harness/sprint1_probes.py preflight
  python tools/harness/sprint1_probes.py profile --build
  python tools/harness/sprint1_probes.py manifest
  python tools/harness/sprint1_probes.py plan
  python tools/harness/sprint1_probes.py compare --probes <manifest.json> --log <events.jsonl> --out <report.json>
  python tools/harness/sprint1_probes.py run --consent   # refused without integrator consent
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LANE = REPO_ROOT / "artifacts" / "lanes" / "s1-06-dynamic"
RESOURCES = REPO_ROOT / "tools" / "resources"
MOD = REPO_ROOT / "tools" / "mod"
for _p in (str(RESOURCES), str(MOD), str(REPO_ROOT / "tools" / "inventory")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

BUILD_SHA256 = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6"
PATCHED_SHA256 = "280ba024d7410933ef1dad5ed829e9bef428c07e7ae47d8a6ad7a3a9420d49e3"
IMAGE_BASE = 0x00400000

DEFAULT_INSTALL = Path("/e/SteamLibrary/steamapps/common/swkotor")

# Read-only sentinel files at the install top level (verified 2026-10-26 against
# artifacts/lanes/01-manifest/manifest.jsonl and docs/recon/16_WINDOWED_1280.md).
SENTINELS = {
    "swkotor.exe": BUILD_SHA256,
    "patch.erf": "0eecf9f93bd5487189582d355f23fc861b7c71f693555e00dbc34ec9d8481364",
    "swkotor.ini": "85b1c89b4acb299e4de65633784fef56b22e64c93ea5064948c97bb3e8c059ef",
    "chitin.key": "2029ec1ee3f0e78a951320a7d9e3cfd0c21fc7ad6ec875e5de59a75f1c62df98",
}

# Required [Graphics Options] for the windowed 1280x720 patched copy (docs/recon/16).
PROFILE_INI = """[Display Options]
FullScreen=0
[Graphics Options]
AllowWindowedMode=1
Width=1280
Height=720
RefreshRate=60
Frame Buffer=0
Shadows=0
Soft Shadows=0
Grass=0
Anti Aliasing=0
Texture Quality=2
"""

# Profile entries junctioned (read-only) into the install vs created writable locally.
JUNCTION_DIRS = ("data", "launcher", "lips", "miles", "movies", "rims", "modules",
                 "streammusic", "streamsounds", "streamwaves", "TexturePacks", "utils",
                 "DMVault", "LocalVault", "ServerVault")
JUNCTION_FILES = ("binkw32.dll", "chitin.key", "dialog.tlk", "dxil.dll",
                  "libgallium_wgl.dll", "Mss32.dll", "opengl32.dll", "patch.erf",
                  "Patchw32.dll")
WRITABLE_DIRS = ("Override", "currentgame", "gameinprogress", "logs", "saves",
                 "temp", "Tempclient")

# Hook/observe plan, image base 0x400000, build SHA-256 BUILD_SHA256.
# Addresses cited from docs/recon/08_VFS_DYNAMIC.md (resolver/loaders) and
# docs/recon/12_COORDS_OPEN_ITEMS.md (dialogue camera).
PROBE_PLAN = {
    "schema": "kotor-s1-06-dynamic-plan/1",
    "build_sha256": BUILD_SHA256,
    "image_base": IMAGE_BASE,
    "consent_required": True,
    "measurements": {
        "a_fresh_module_area_rim_order": {
            "description": ("fresh New Game -> character generation -> first module "
                            "(end_m01aa) load: exact resolver open/mount order for "
                            "rims/<area>_a.rim, rims/<area>_adx.rim, modules/<area>.rim, "
                            "modules/<area>_s.rim and modules/<area>.mod"),
            "watch": {
                "loader_erf": 0x40F3C0, "loader_rim": 0x40F990, "loader_key": 0x40FB80,
                "resman_lookup_a": 0x4071A0, "resman_lookup_b": 0x407230,
                "addkey": 0x40E990,
            },
            "targets": ["end_m01aa", "module"],
            "expected_static_model": ("kotor_vfs pass 3 order: <root>_s.rim, <root>_adx.rim, "
                                      "<root>_a.rim, <root>.rim before menu/global "
                                      "(statically proven, docs/recon/23 #23-#25); .mod "
                                      "mounted instead of _s when present (verified for "
                                      "loaded saves)"),
            "status": ("statically closed (docs/recon/23 #23-#25); live observation of "
                       "_s firing remains blocked by the chargen PLAY input path"),
        },
        "b_streamwaves": {
            "description": ("streamwaves/ directory scan + pass-1 index + any (resref, WAV/4) "
                            "requests during menu -> module -> gameplay"),
            "watch": {
                "dir_scan_dirs": ["override", "movies", "streamwaves", "streammusic"],
                "resman_lookup_a": 0x4071A0, "resman_lookup_b": 0x407230,
                "addkey": 0x40E990,
                "file_open_wav_ext": ".wav",
            },
            "wav_type": 4,
            "status": "open (directory scan and pass-1 index measured in lane 06; audio "
                      "requests unobserved)",
        },
        "c_dialogue_camera": {
            "description": ("start a dialogue in the Taris Hideout save (tar_m02af) via normal "
                            "UI input; observe camera object +0x38 and dialogue controller "
                            "+0xc4 live in-process"),
            "watch": {
                "camera_ctor": 0x6BB670,          # writes camera +0x38 at 0x6bb6f8
                "camera_mode_write": 0x6BB6F8,
                "controller_ctor": 0x62FED0,      # zeroes controller +0xc4 at 0x630244
                "controller_mode_write": 0x630244,
                "camera_factory": 0x6412F0,      # reads controller +0xc4 at 0x641362-0x64136f
                "camera_mode_reads": [0x62BF4E, 0x6320C9, 0x632127],
            },
            "fields": {"camera_object": "+0x38", "dialogue_controller": "+0xc4"},
            "status": "open (controller +0xc4=0 observed live in a non-dialogue scene; camera "
                      "object +0x38 not yet observed in a dialogue)",
        },
        "d_save_acceptance": {
            "description": ("if a retail save exists: s1-05 rewritten-save acceptance check "
                            "(original loads the rewritten save; F5 reload)"),
            "status": "open (install has no saves/ slots; lane-owned downloaded "
                      "'000005 - TARIS' exists under artifacts/lanes/06-dynamic/profile/saves)",
        },
    },
}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def game_dir_arg(args) -> Path:
    raw = args.game_dir or os.environ.get('KOTOR_GAME_DIR') or str(DEFAULT_INSTALL)
    fwd = raw.replace(chr(92), '/')
    candidates = [Path(raw)]
    if len(fwd) > 2 and fwd[0] == '/' and fwd[2] == '/':
        # Git Bash drive path (/e/... -> E:/...; Windows APIs accept forward slashes)
        candidates.append(Path(fwd[1].upper() + ':' + fwd[2:]))
    for g in candidates:
        if (g / 'chitin.key').exists():
            return g
    raise SystemExit(f'game install not found at {raw} (set KOTOR_GAME_DIR)')


def guard_not_in_install(path: Path, install: Path) -> None:
    p = path.resolve()
    inst = install.resolve()
    if p == inst or inst in p.parents:
        raise SystemExit(f"refusing to write inside the game install: {p}")


def game_processes() -> list[dict]:
    ps = ("Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'swkotor.exe' } | "
          "Select-Object ProcessId,ParentProcessId,Name,ExecutablePath | ConvertTo-Json -Compress")
    r = subprocess.run(["powershell.exe", "-NoProfile", "-Command", ps],
                       capture_output=True, text=True)
    out = r.stdout.strip()
    if not out or out == "null":
        return []
    data = json.loads(out)
    return data if isinstance(data, list) else [data]


def cmd_preflight(args) -> int:
    install = game_dir_arg(args)
    LANE.mkdir(parents=True, exist_ok=True)
    report: dict = {
        "schema": "kotor-s1-06-preflight/1",
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "install": str(install),
        "build_sha256_expected": BUILD_SHA256,
        "sentinels": {},
        "processes": game_processes(),
        "manifest_crosscheck": {"checked": 0, "drift": []},
        "ok": True,
    }
    for name, expected in SENTINELS.items():
        p = install / name
        if not p.exists():
            report["sentinels"][name] = {"status": "missing"}
            report["ok"] = False
            continue
        got = sha256_file(p)
        status = "ok" if got == expected else "DRIFT"
        if got != expected:
            report["ok"] = False
        report["sentinels"][name] = {"sha256": got, "expected": expected, "status": status}
    # Cross-check top-level config files against the lane 01 manifest (read-only).
    manifest = REPO_ROOT / "artifacts" / "lanes" / "01-manifest" / "manifest.jsonl"
    if manifest.exists():
        with manifest.open(encoding="utf-8") as f:
            for line in f:
                try:
                    e = json.loads(line)
                except json.JSONDecodeError:
                    continue
                rel = str(e.get("path", ""))
                if "/" in rel or "\\" in rel:
                    continue
                if e.get("kind") != "config" and not rel.lower().endswith(
                        (".ini", ".cfg", ".conf", ".vdf")):
                    continue
                p = install / rel
                if not p.is_file():
                    continue
                report["manifest_crosscheck"]["checked"] += 1
                got = sha256_file(p)
                if got != e.get("sha256"):
                    report["manifest_crosscheck"]["drift"].append(
                        {"path": rel, "sha256": got, "expected": e.get("sha256")})
                    report["ok"] = False
    report["single_instance_ok"] = len(report["processes"]) == 0
    report["consent_file"] = (LANE / "CONSENT").exists()
    out = LANE / "preflight.json"
    guard_not_in_install(out, install)
    out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"ok": report["ok"],
                      "sentinels": {k: v["status"] for k, v in report["sentinels"].items()},
                      "manifest_checked": report["manifest_crosscheck"]["checked"],
                      "manifest_drift": len(report["manifest_crosscheck"]["drift"]),
                      "game_processes": len(report["processes"]),
                      "consent_file": report["consent_file"],
                      "out": str(out)}, indent=2))
    if not report["ok"]:
        print("STOP: install sentinel drift detected; not proceeding", file=sys.stderr)
        return 3
    return 0


def _mklink(kind: str, dst: Path, src: Path) -> None:
    if dst.exists() or dst.is_symlink():
        return
    r = subprocess.run(["cmd", "/c", "mklink", kind, str(dst), str(src)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"mklink failed for {dst}: {r.stdout} {r.stderr}")


def cmd_profile(args) -> int:
    install = game_dir_arg(args)
    # Sentinel first: stop on drift before touching anything.
    rc = cmd_preflight(args)
    if rc != 0:
        return rc
    profile = LANE / "profile"
    guard_not_in_install(profile, install)
    profile.mkdir(parents=True, exist_ok=True)
    steps: list[str] = []

    exe = profile / "swkotor.exe"
    if not exe.exists() or sha256_file(exe) not in (BUILD_SHA256, PATCHED_SHA256):
        shutil.copy2(install / "swkotor.exe", exe)
        steps.append("copied swkotor.exe from install")
    if sha256_file(exe) == BUILD_SHA256:
        import kotor_windowed_1280 as w
        manifest = w.apply(exe)  # patches the COPY only; verifies original hash first
        steps.append(f"applied windowed-1280 patch to copy: {manifest['patched_sha256']}")
    got = sha256_file(exe)
    if got != PATCHED_SHA256:
        raise SystemExit(f"profile exe hash {got} != expected patched {PATCHED_SHA256}")
    steps.append("profile exe verified patched " + PATCHED_SHA256[:16] + "...")

    ini = profile / "swkotor.ini"
    if not ini.exists():
        ini.write_text(PROFILE_INI, encoding="utf-8")
        steps.append("wrote profile swkotor.ini (FullScreen=0, 1280x720)")

    for d in JUNCTION_DIRS:
        src = install / d
        if src.is_dir():
            _mklink("/J", profile / d, src)
    for f in JUNCTION_FILES:
        src = install / f
        if src.is_file():
            _mklink("/H", profile / f, src)
    for d in WRITABLE_DIRS:
        (profile / d).mkdir(exist_ok=True)
    steps.append("junctioned read-only install data; created writable profile dirs")

    # Fresh save slots only: profile saves dir starts empty; never touch install saves.
    saves = profile / "saves"
    existing = sorted(p.name for p in saves.iterdir())
    report = {
        "schema": "kotor-s1-06-profile/1",
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "profile": str(profile),
        "install": str(install),
        "exe_sha256": got,
        "exe_status": "patched-windowed-1280",
        "install_sentinel_ok": True,
        "saves": existing,
        "steps": steps,
    }
    if args.import_save:
        src = Path(args.import_save)
        if not src.is_dir():
            raise SystemExit(f"--import-save must be a save slot folder: {src}")
        guard_not_in_install(src, install)
        # fresh slot: first free NNNNNN index, original slot never overwritten
        used = {p.name.split(" ")[0] for p in saves.iterdir() if p.is_dir()}
        n = 0
        while f"{n:06d}" in used:
            n += 1
        name = f"{n:06d} - {src.name.split(' - ', 1)[-1]}"
        dst = saves / name
        shutil.copytree(src, dst)
        report["imported_save"] = {"from": str(src), "to": str(dst)}
        steps.append(f"imported save as fresh slot {name!r} (source untouched)")
    out = LANE / "profile.json"
    out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"profile": str(profile), "exe": got[:16] + "...",
                      "saves": report["saves"], "steps": steps, "out": str(out)}, indent=2))
    return 0


def cmd_manifest(args) -> int:
    install = game_dir_arg(args)
    import build_dynamic_probes as bdp
    outdir = LANE / "manifests"
    guard_not_in_install(outdir, install)
    outdir.mkdir(parents=True, exist_ok=True)
    summary = {}
    for module in args.modules:
        m = bdp.build(install, module, None, args.max_per_class, args.max_total)
        out = outdir / f"probes-{module}.json"
        out.write_text(json.dumps(m, indent=2), encoding="utf-8")
        summary[module] = m["counts"]
    # streamwaves sample manifest: (resref, WAV/4) keys expected in pass-1 streamwaves.
    sw = install / "streamwaves"
    wavs = sorted(p.name for p in sw.glob("*.wav")) if sw.is_dir() else []
    sample = wavs[:: max(1, len(wavs) // 48)][:48]
    sw_manifest = {
        "schema": "kotor-s1-06-streamwaves/1",
        "build_sha256": BUILD_SHA256,
        "dir": "streamwaves",
        "total_wav": len(wavs),
        "sampled": len(sample),
        "probes": [
            {"id": f"streamwaves.{Path(w).stem}.4", "kind": "streamwaves",
             "key": {"resref": Path(w).stem.lower(), "type": 4, "type_label": "WAV"},
             "sources": [{"pass": 1, "tier": "directory:HD0:STREAMWAVES",
                          "table": "HD0:STREAMWAVES", "source": f"streamwaves/{w}",
                          "size": (sw / w).stat().st_size}],
             "expected": {"found": True, "pass": 1, "table": "HD0:STREAMWAVES"},
             "authority": "inferred (directory scan + pass-1 index measured; per-file audio "
                          "requests unobserved)",
             "observation": {"method": "log (resref,4) resman lookups and .wav file opens",
                             "match": ["table", "size"]}}
            for w in sample
        ],
    }
    (outdir / "probes-streamwaves.json").write_text(
        json.dumps(sw_manifest, indent=2), encoding="utf-8")
    summary["streamwaves"] = {"total_wav": len(wavs), "sampled": len(sample)}
    print(json.dumps(summary, indent=2))
    print(f"wrote manifests under {outdir}")
    return 0


def cmd_plan(args) -> int:
    install = game_dir_arg(args)
    LANE.mkdir(parents=True, exist_ok=True)
    plan = dict(PROBE_PLAN)
    plan["generated_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    plan["install"] = str(install)
    plan["consent"] = {
        "file_present": (LANE / "CONSENT").exists(),
        "note": ("display consent withheld at prep time; the game is NOT launched until the "
                 "integrator's consent message places artifacts/lanes/s1-06-dynamic/CONSENT"),
    }
    plan["run_driver"] = ("live session driven via run_typescript -> tools.mcp_hydralysis.runtime "
                          "(spawn/attach target kotor, build sha256 " + BUILD_SHA256 + ") using "
                          "this plan; one owned instance only; capture only the identified game window")
    out = LANE / "probe_plan.json"
    guard_not_in_install(out, install)
    out.write_text(json.dumps(plan, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"out": str(out), "measurements": sorted(plan["measurements"].keys()),
                      "consent_file": plan["consent"]["file_present"]}, indent=2))
    return 0


def cmd_compare(args) -> int:
    import vfs_measure as vm
    probes = json.loads(Path(args.probes).read_text(encoding="utf-8"))
    events = vm.load_events(Path(args.log))
    report = vm.compare(probes, events)
    out = Path(args.out)
    install = game_dir_arg(args)
    guard_not_in_install(out, install)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"counts": report["counts"],
                      "texture_quality_counts": report["texture_quality_counts"],
                      "out": str(out)}, indent=2))
    return 1 if report["counts"].get("mismatch") else 0


def cmd_run(args) -> int:
    consent = LANE / "CONSENT"
    procs = game_processes()
    if not args.consent or not consent.exists():
        print("REFUSED: no display consent. The game is not launched. "
              "Wait for the integrator's consent message (CONSENT file under "
              f"{LANE}) and re-run with --consent.", file=sys.stderr)
        return 4
    if procs:
        print(f"REFUSED: swkotor.exe already running ({len(procs)} instance(s)); "
              "one owned instance at a time.", file=sys.stderr)
        return 4
    print("Consent file present and no other instance running. This script still does not "
          "launch the game: drive the owned session via run_typescript -> "
          "tools.mcp_hydralysis.runtime with " + str(LANE / "probe_plan.json"))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game-dir", default=None,
                    help="install root (default $KOTOR_GAME_DIR or the Steam default)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("preflight")
    p = sub.add_parser("profile")
    p.add_argument("--build", action="store_true")
    p.add_argument("--import-save", default=None,
                   help="copy an existing save slot folder into a FRESH profile slot")
    sub.add_parser("manifest").add_argument  # placeholder replaced below
    m = sub.choices["manifest"]
    m.add_argument("--modules", nargs="+", default=["end_m01aa", "tar_m02af"])
    m.add_argument("--max-per-class", type=int, default=4)
    m.add_argument("--max-total", type=int, default=200)
    sub.add_parser("plan")
    c = sub.add_parser("compare")
    c.add_argument("--probes", required=True)
    c.add_argument("--log", required=True)
    c.add_argument("--out", required=True)
    r = sub.add_parser("run")
    r.add_argument("--consent", action="store_true")
    args = ap.parse_args(argv)
    if args.cmd == "preflight":
        return cmd_preflight(args)
    if args.cmd == "profile":
        if not args.build:
            ap.error("profile requires --build")
        return cmd_profile(args)
    if args.cmd == "manifest":
        return cmd_manifest(args)
    if args.cmd == "plan":
        return cmd_plan(args)
    if args.cmd == "compare":
        return cmd_compare(args)
    if args.cmd == "run":
        return cmd_run(args)
    return 2


if __name__ == "__main__":
    sys.exit(main())
