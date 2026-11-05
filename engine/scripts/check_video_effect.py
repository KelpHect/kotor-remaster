#!/usr/bin/env python3
"""Verification-only GPU gate for the videoeffects.2da post-process (ACT_508/510).

Not part of the runtime. Per backend it launches ONE engine process that renders
a warm-up (the module opens with a scripted fade from black), then runs the
compiled video_effect_*.ncs fixtures through the real script VM, one per frame
(--video-effect-sequence), and writes a BMP after each sequence frame
(--video-effect-shots). Because every state is captured in the same process a
few frames apart, only the effect differs between the shots.

  shot 0,1  keep, keep         baseline and the frame-to-frame noise floor
  shot 2    EnableVideoEffect(0)
  shot 3    EnableVideoEffect(1)
  shot 4    EnableVideoEffect(2)
  shot 5    DisableVideoEffect()  must return to the baseline
  shot 6    keep                  still the baseline

It then checks that every enabled row changes the frame on both backends, that
disabling restores it, and that GL and Vulkan agree about how much each row
changes it. The HUD (top-left) is excluded. Requires Pillow.

Usage:
  python engine/scripts/check_video_effect.py
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

try:
    from PIL import Image, ImageChops, ImageStat
except ImportError:
    print("Pillow is required (pip install pillow)", file=sys.stderr)
    raise SystemExit(2)

ENGINE = Path(__file__).resolve().parents[1]
DEFAULT_EXE = ENGINE / "build" / "msvc-release" / "kotor_engine.exe"
DEFAULT_OUT = ENGINE / "artifacts" / "engine" / "video_effect"
FIXTURES = ENGINE / "tests" / "fixtures"

SEQUENCE = ["keep", "keep", "row0", "row1", "row2", "off", "keep"]
NAMES = {0: "baseline", 1: "baseline-2", 2: "row0", 3: "row1", 4: "row2", 5: "disabled", 6: "after"}


def run(exe: Path, module: str, backend: str, warmup: int, width: int, height: int, out: Path):
    prefix = out / f"video-{backend}"
    command = [
        str(exe), "--module", module, f"--{backend}",
        "--width", str(width), "--height", str(height),
        "--frames", str(warmup + len(SEQUENCE)),
        "--video-effect-fixtures", str(FIXTURES),
        "--video-effect-sequence", ",".join(SEQUENCE),
        "--video-effect-warmup", str(warmup),
        "--video-effect-shots", str(prefix),
    ]
    print("+", " ".join(command))
    subprocess.run(command, cwd=ENGINE, check=True)
    shots = []
    for index in range(len(SEQUENCE)):
        path = Path(f"{prefix}-{index}.bmp")
        if not path.is_file():
            raise SystemExit(f"capture missing: {path}")
        shots.append(Image.open(path).convert("RGB"))
    return shots


def difference(a: Image.Image, b: Image.Image, box) -> float:
    stats = ImageStat.Stat(ImageChops.difference(a.crop(box), b.crop(box)))
    return sum(stats.mean) / 3.0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Video effect GPU gate (GL and Vulkan)")
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--module", default="end_m01aa")
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=540)
    parser.add_argument("--warmup", type=int, default=400)
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    parser.add_argument("--visible", type=float, default=2.0,
                        help="minimum mean abs channel change an enabled effect must cause")
    parser.add_argument("--restore", type=float, default=1.0,
                        help="maximum mean abs channel difference after disabling (also scaled "
                             "by the measured frame-to-frame floor)")
    parser.add_argument("--agree", type=float, default=0.35,
                        help="maximum relative difference between the GL and Vulkan change sizes")
    args = parser.parse_args(argv)

    exe = Path(args.exe).resolve()
    if not exe.is_file():
        raise SystemExit(f"engine not found: {exe}")
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    box = (int(args.width * 0.52), int(args.height * 0.22), args.width - 20, args.height - 20)

    shots = {backend: run(exe, args.module, backend, args.warmup, args.width, args.height, out)
             for backend in ("vulkan", "gl")}

    verdict = True
    lines = [f"module={args.module}", f"resolution={args.width}x{args.height}",
             f"warmup_frames={args.warmup}", f"scene_region={box}"]
    change: dict[str, dict[str, float]] = {}
    for backend, images in shots.items():
        floor = difference(images[0], images[1], box)
        restore_limit = max(args.restore, 3.0 * floor)
        lines.append(f"{backend}: frame-to-frame floor (shot0 vs shot1) mean_abs_diff={floor:.4f}")
        change[backend] = {}
        for index, row in ((2, "row0"), (3, "row1"), (4, "row2")):
            value = difference(images[1], images[index], box)
            change[backend][row] = value
            ok = value >= args.visible
            verdict &= ok
            lines.append(f"{backend}: {row} enabled vs baseline mean_abs_diff={value:.4f} "
                         f"(min {args.visible}) {'ok' if ok else 'FAIL'}")
        for index in (5, 6):
            value = difference(images[1], images[index], box)
            ok = value <= restore_limit
            verdict &= ok
            lines.append(f"{backend}: {NAMES[index]} vs baseline mean_abs_diff={value:.4f} "
                         f"(limit {restore_limit:.3f}) {'ok' if ok else 'FAIL'}")
    for row in ("row0", "row1", "row2"):
        a, b = change["vulkan"][row], change["gl"][row]
        relative = abs(a - b) / max(a, b, 1e-6)
        ok = relative <= args.agree
        verdict &= ok
        lines.append(f"agreement {row}: vulkan change {a:.3f}, gl change {b:.3f}, relative difference "
                     f"{relative:.3f} (limit {args.agree}) {'ok' if ok else 'FAIL'}")
    lines.append(f"verdict={'PASS' if verdict else 'FAIL'}")
    report = "\n".join(lines) + "\n"
    (out / "video-effect-report.txt").write_text(report, encoding="utf-8")
    print(report)
    return 0 if verdict else 1


if __name__ == "__main__":
    raise SystemExit(main())
