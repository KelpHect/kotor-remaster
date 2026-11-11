#!/usr/bin/env python3
"""GL and Vulkan draw the same world: same shots on one fixed clock, compared pixel by pixel.

A per-frame mean hides a small local fault, so the worst 32x32 block is measured as well. A backend
that fails to start, or a frame with almost no colours (two blank frames match), fails the gate.
"""
from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SHOTS = [
    ("door_swing", ["--module", "end_m01aa", "--capture-camera", "door:0", "--capture-action", "door:0@50"], 110),
    ("coplanar_trim", ["--module", "STUNT_03a", "--capture-camera", "creature:1"], 90),
    ("fire", ["--module", "tar_m04aa", "--capture-camera", "emitter:smfire:0"], 150),
    ("grass_sky", ["--module", "danm13", "--capture-camera", "entry"], 90),
    ("env_map", ["--module", "lev_m40aa", "--capture-camera", "entry"], 90),
]
MAX_MEAN = 1.0
MAX_BLOCK = 8.0
MIN_COLOURS = 64
BLOCK = 32


def read_bmp(path: Path) -> tuple[int, int, int, bytes]:
    data = path.read_bytes()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    return width, abs(height), bits // 8, data[offset:]


def capture(args, backend: str, name: str, shot_args: list[str], frames: int, folder: Path) -> Path | None:
    bmp = folder / f"{name}_{backend}.bmp"
    cmd = [args.exe, f"--{'opengl' if backend == 'gl' else 'vulkan'}", "--no-fallback", "--install", args.install,
           "--width", "1280", "--height", "720", "--frames", str(frames), "--fixed-dt", "0.0166667",
           "--no-audio", "--no-movies", "--screenshot", str(bmp)] + shot_args
    if args.saves_dir:
        cmd += ["--saves-dir", args.saves_dir]
    if args.settings_file:
        cmd += ["--settings-file", args.settings_file]
    code = subprocess.run(cmd, capture_output=True, timeout=240, check=False).returncode
    if code != 0 or not bmp.exists():
        print(f"FAIL {name}: {backend} exit {code}")
        return None
    return bmp


def compare(gl_path: Path, vk_path: Path) -> tuple[float, float, int]:
    width, height, step, a = read_bmp(gl_path)
    other_width, other_height, other_step, b = read_bmp(vk_path)
    if (width, height, step) != (other_width, other_height, other_step):
        return 255.0, 255.0, 0
    stride = (width * step + 3) & ~3
    columns, rows = width // BLOCK, height // BLOCK
    blocks = [0] * (columns * rows)
    colours = set()
    total = 0
    for y in range(rows * BLOCK):
        row_a, row_b = a[y * stride:y * stride + width * step], b[y * stride:y * stride + width * step]
        colours.update(row_a[x:x + 3] for x in range(0, len(row_a), step * 16))
        base = (y // BLOCK) * columns
        if row_a == row_b:
            continue
        for x in range(columns * BLOCK):
            i = x * step
            delta = abs(row_a[i] - row_b[i]) + abs(row_a[i + 1] - row_b[i + 1]) + abs(row_a[i + 2] - row_b[i + 2])
            if delta:
                blocks[base + x // BLOCK] += delta
                total += delta
    samples = 3 * BLOCK * BLOCK
    return total / (samples * columns * rows), max(blocks) / samples, len(colours)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--install", required=True)
    parser.add_argument("--saves-dir", help="passed to every engine run, so none touches the user's saves")
    parser.add_argument("--settings-file", help="passed to every engine run, so none touches the user's settings")
    args = parser.parse_args()
    failed = 0
    with tempfile.TemporaryDirectory() as temp:
        folder = Path(temp)
        for name, shot_args, frames in SHOTS:
            gl_path = capture(args, "gl", name, shot_args, frames, folder)
            vk_path = capture(args, "vulkan", name, shot_args, frames, folder)
            if gl_path is None or vk_path is None:
                failed += 1
                continue
            mean, block, colours = compare(gl_path, vk_path)
            ok = mean <= MAX_MEAN and block <= MAX_BLOCK and colours >= MIN_COLOURS
            print(f"{'PASS' if ok else 'FAIL'} {name}: mean {mean:.3f} worst block {block:.2f} colours {colours}")
            failed += 0 if ok else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
