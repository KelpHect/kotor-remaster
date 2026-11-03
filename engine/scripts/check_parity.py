#!/usr/bin/env python3
"""Verification-only GL/Vulkan scene parity check for the KotOR slice.

This is not part of the runtime or the asset pipeline. It launches the C++
engine twice with the same module/camera, captures a BMP from each backend,
and compares the scene region (the HUD is excluded because its FPS text
changes every run). Requires Pillow.

Usage:
  python engine/scripts/check_parity.py
  python engine/scripts/check_parity.py --module end_m01aa --threshold 1.0
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

try:
    from PIL import Image, ImageChops, ImageStat
except ImportError:
    print("Pillow is required for the parity check (pip install pillow)", file=sys.stderr)
    raise SystemExit(2)

ENGINE = Path(__file__).resolve().parents[1]
DEFAULT_EXE = ENGINE / "build" / "msvc-release" / "kotor_engine.exe"
DEFAULT_OUT = ENGINE / "artifacts" / "engine" / "parity"


def capture(exe: Path, module: str, backend: str, width: int, height: int, out: Path) -> Path:
    bmp = out / f"parity-{backend}.bmp"
    command = [
        str(exe),
        "--module", module,
        f"--{backend}",
        "--width", str(width),
        "--height", str(height),
        "--frames", "3",
        "--screenshot", str(bmp),
    ]
    print("+", " ".join(command))
    subprocess.run(command, cwd=ENGINE, check=True)
    if not bmp.is_file():
        raise SystemExit(f"capture missing: {bmp}")
    return bmp


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Compare the GL and Vulkan scene captures")
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--module", default="end_m01aa")
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=540)
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    parser.add_argument("--threshold", type=float, default=1.0,
                        help="maximum mean absolute channel difference in the scene region")
    args = parser.parse_args(argv)

    exe = Path(args.exe)
    if not exe.is_file():
        raise SystemExit(f"engine not found: {exe}")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    gl_bmp = capture(exe, args.module, "gl", args.width, args.height, out)
    vk_bmp = capture(exe, args.module, "vulkan", args.width, args.height, out)
    gl_png = out / "parity-gl.png"
    vk_png = out / "parity-vk.png"
    gl = Image.open(gl_bmp).convert("RGB")
    vk = Image.open(vk_bmp).convert("RGB")
    gl.save(gl_png)
    vk.save(vk_png)

    # Exclude the HUD (top-left) so only the world is compared.
    box = (int(args.width * 0.52), int(args.height * 0.22), args.width - 20, args.height - 20)
    gl_scene = gl.crop(box)
    vk_scene = vk.crop(box)
    diff = ImageChops.difference(gl_scene, vk_scene)
    stats = ImageStat.Stat(diff)
    mean = sum(stats.mean) / 3.0
    rms = sum(stats.rms) / 3.0
    diff.save(out / "parity-diff.png")

    verdict = "PASS" if mean <= args.threshold else "FAIL"
    report = (
        f"module={args.module}\n"
        f"resolution={args.width}x{args.height}\n"
        f"scene_region={box}\n"
        f"gl_capture={gl_png}\n"
        f"vulkan_capture={vk_png}\n"
        f"mean_abs_channel_diff={mean:.4f}\n"
        f"rms_channel_diff={rms:.4f}\n"
        f"threshold={args.threshold:.4f}\n"
        f"verdict={verdict}\n"
    )
    (out / "parity-report.txt").write_text(report, encoding="utf-8")
    print(report)
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
