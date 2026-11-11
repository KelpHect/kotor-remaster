#!/usr/bin/env python3
"""GPU gate: the mouth on screen changes with the lip shape, and only the mouth.

Renders the shipped Trask head in close-up three times, identical but for the held lip shape
(the same frame count and fixed step, so the idle clip is at the same moment), and compares the
crop around the mouth with the crop around the brow. Requires Pillow.

Usage: python engine/scripts/check_talk_pixels.py [--exe PATH] [--out DIR] [--backend gl|vulkan]
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
WIDTH, HEIGHT = 1280, 720
# Where the mouth is on screen depends on the dialogue pose and the capture camera, which other work
# moves (the first measurement had it at y 0.60, a later pose at 0.77). The boxes are therefore placed
# from the picture: the lower face is where the held shapes differ (the jaw moves with the lips, so
# the centre of the difference sits 0.04 of the height below the lips), the brow is 0.33 above the lips.
MOUTH_HALF = (0.06, 0.065)   # half width and height of the mouth box, fractions of the picture
BROW_HALF = (0.07, 0.035)
JAW_BELOW_LIPS = 0.04
BROW_ABOVE_LIPS = 0.33
DIFFERS = 30                 # summed channel difference that counts as a changed pixel
MIN_MOUTH_CHANGE = 3.0  # Mean level of 255; the shipped head measures 5.3 and 7.1 on GL and Vulkan.
MAX_BROW_RATIO = 0.25   # The brow moves far less than the mouth.
MIN_FOCUS = 0.6         # Share of the changed pixels that lie in the (doubled) mouth box: a mouth change is local.


def shot(exe: Path, out: Path, shape: int, install: str | None, backend: str = "gl",
         saves_dir: str | None = None, settings_file: str | None = None) -> Image.Image:
    bmp = out / f"talk-{backend}-shape-{shape}.bmp"
    bmp.unlink(missing_ok=True)
    command = [str(exe), "--module", "end_m01aa", f"--{backend}", "--no-fallback", "--width", str(WIDTH), "--height", str(HEIGHT),
               "--frames", "16", "--fixed-dt", "0.05", "--screenshot", str(bmp), "--no-audio", "--no-movies",
               "--demo-dialogue", "--capture-camera", "node:end_trask:headhook:0.5",
               "--capture-action", f"lip:{shape}@5"]
    if install:
        command += ["--install", install]
    if saves_dir:
        command += ["--saves-dir", saves_dir]
    if settings_file:
        command += ["--settings-file", settings_file]
    subprocess.run(command, cwd=ENGINE, check=True, timeout=180, stdin=subprocess.DEVNULL,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    if not bmp.is_file():
        raise SystemExit(f"capture missing: {bmp}")
    return Image.open(bmp).convert("RGB")


def change(a: Image.Image, b: Image.Image, box) -> float:
    pixels = (int(box[0] * a.width), int(box[1] * a.height), int(box[2] * a.width), int(box[3] * a.height))
    stats = ImageStat.Stat(ImageChops.difference(a.crop(pixels), b.crop(pixels)))
    return sum(stats.mean) / len(stats.mean)


def locate_mouth(closed: Image.Image, others: list[Image.Image]):
    """Mouth and brow boxes from where the held shapes differ, and the share of the changed pixels
    inside the doubled mouth box; None when nothing moved."""
    xs = ys = weight = 0.0
    points = []
    for other in others:
        diff = ImageChops.difference(closed, other)
        width, height = diff.size
        data = diff.load()
        for y in range(0, height, 2):
            for x in range(0, width, 2):
                value = sum(data[x, y])
                if value > DIFFERS:
                    xs += x * value
                    ys += y * value
                    weight += value
                    points.append((x, y))
    if weight <= 0.0:
        return None
    cx = xs / weight / closed.width
    cy = ys / weight / closed.height - JAW_BELOW_LIPS
    mouth = (cx - MOUTH_HALF[0], cy - MOUTH_HALF[1], cx + MOUTH_HALF[0], cy + MOUTH_HALF[1])
    by = cy - BROW_ABOVE_LIPS
    brow = (cx - BROW_HALF[0], by - BROW_HALF[1], cx + BROW_HALF[0], by + BROW_HALF[1])
    mid_y = cy + JAW_BELOW_LIPS
    inside = sum(1 for x, y in points
                 if abs(x / closed.width - cx) <= 2 * MOUTH_HALF[0] and abs(y / closed.height - mid_y) <= 2 * MOUTH_HALF[1])
    return mouth, brow, inside / len(points)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--out", type=Path, default=ENGINE / "artifacts" / "engine" / "talk_pixels")
    parser.add_argument("--install")
    parser.add_argument("--backend", choices=["gl", "vulkan"], default="gl")
    parser.add_argument("--saves-dir", help="passed to every engine run, so none touches the user's saves")
    parser.add_argument("--settings-file", help="passed to every engine run, so none touches the user's settings")
    parser.add_argument("--control-still", action="store_true",
                        help="negative control: hold the closed mouth in every shot, so the check must FAIL")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    closed = shot(args.exe, args.out, 11, args.install, args.backend, args.saves_dir, args.settings_file)
    closed_again = shot(args.exe, args.out, 11, args.install, args.backend, args.saves_dir, args.settings_file)
    opened = shot(args.exe, args.out, 11 if args.control_still else 13, args.install, args.backend, args.saves_dir, args.settings_file)
    other = shot(args.exe, args.out, 11 if args.control_still else 3, args.install, args.backend, args.saves_dir, args.settings_file)
    boxes = locate_mouth(closed, [opened, other])
    if boxes is None:
        print("mouth change: no pixel differs between the held shapes")
        print("talk pixels: FAIL")
        return 1
    mouth_box, brow_box, focus = boxes
    print("mouth box %.3f %.3f %.3f %.3f, brow box %.3f %.3f %.3f %.3f" % (mouth_box + brow_box))
    print(f"changed pixels inside the doubled mouth box {focus:.2f}")
    noise = change(closed, closed_again, mouth_box)
    roar = change(closed, opened, mouth_box)
    ox = change(closed, other, mouth_box)
    brow = change(closed, opened, brow_box)
    print(f"mouth change bump-roar {roar:.2f} bump-ox {ox:.2f}, repeat noise {noise:.2f}, brow {brow:.2f}")
    ok = (roar >= MIN_MOUTH_CHANGE and ox >= MIN_MOUTH_CHANGE and roar > noise * 3.0
          and brow <= roar * MAX_BROW_RATIO and focus >= MIN_FOCUS)
    print("talk pixels: PASS" if ok else "talk pixels: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
