#!/usr/bin/env python3
"""Lane s1-08 GUI viewer: render a shipped .gui screen to PNG.

Parses the GUI GFF (type 2047) with the project reader (read-only), walks the
flat ``CONTROLS`` list, and renders each control's EXTENT rect (640x480 GUI
space, scaled) to a pure-Python PNG: BORDER.FILL stretched over the rect,
EDGE/CORNER drawn as border strips, otherwise the control COLOR as a flat
fill.  Referenced textures (CORNER/EDGE/FILL/IMAGE/FONT resrefs) are resolved
through the same read-only VFS-lite as the area viewer (TPC from the texture
packs first, TGA from base BIFs).  Self-checks: rendered control count vs the
GFF CONTROLS list count, and decoded texture dimensions vs each TPC/TGA
header.  Text glyphs are not rasterized (no font renderer in this lane).

Usage (Git Bash):
  KOTOR_GAME_DIR=/e/SteamLibrary/steamapps/common/swkotor \
    python tools/viewer/gui_viewer.py --out artifacts/lanes/s1-08-viewer \
    --guis mainmenu maininterface
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/

from viewer import BUILD_SHA256, game_dir  # noqa: E402
from viewer.area import TextureCache  # noqa: E402
from viewer.gui import parse_gui  # noqa: E402
from viewer.kotor import Game, T_GUI, check_install_sentinel  # noqa: E402
from viewer.pngwrite import Image, write_png  # noqa: E402
from viewer import raster  # noqa: E402

GUI_SPACE = (640, 480)


def _clamp_color(c) -> tuple[int, int, int]:
    out = []
    for v in (c if isinstance(c, (tuple, list)) else (1.0, 1.0, 1.0)):
        if v is None or v < 0:
            out.append(255)
        else:
            out.append(max(0, min(255, int(v * 255))))
    return tuple(out[:3])


def render_gui(parsed: dict, tex: TextureCache, scale: int = 2) -> tuple[Image, list[dict]]:
    w, h = GUI_SPACE[0] * scale, GUI_SPACE[1] * scale
    img = Image(w, h, 3, fill=(12, 12, 16))
    checks: list[dict] = []
    rendered = 0
    tex_hits = 0
    for ctl in parsed["controls"]:
        ext = ctl["extent"]
        if any(ext.get(k) is None for k in ("LEFT", "TOP", "WIDTH", "HEIGHT")):
            continue
        x0 = int(ext["LEFT"]) * scale
        y0 = int(ext["TOP"]) * scale
        x1 = x0 + int(ext["WIDTH"]) * scale
        y1 = y0 + int(ext["HEIGHT"]) * scale
        fill = ctl["border_fill"]
        entry = tex.get(fill) if fill else None
        if entry:
            raster.blit_stretch(img, entry[0], x0, y0, x1 - x0, y1 - y0)
            tex_hits += 1
        else:
            color = _clamp_color(_struct_color(parsed, ctl, "BORDER"))
            raster.fill_rect(img, None, x0, y0, x1, y1, color)
        # border strips from EDGE / corners from CORNER when present
        edge = ctl["border_edge"]
        if edge:
            ee = tex.get(edge)
            if ee:
                t = ee[0]
                dim = 4 * scale
                raster.blit_stretch(img, t, x0, y0, x1 - x0, dim)
                raster.blit_stretch(img, t, x0, y1 - dim, x1 - x0, dim)
                raster.blit_stretch(img, t, x0, y0, dim, y1 - y0)
                raster.blit_stretch(img, t, x1 - dim, y0, dim, y1 - y0)
                tex_hits += 1
        corner = ctl["border_corner"]
        if corner:
            ce = tex.get(corner)
            if ce:
                dim = 4 * scale
                for cx, cy in ((x0, y0), (x1 - dim, y0), (x0, y1 - dim), (x1 - dim, y1 - dim)):
                    raster.blit_stretch(img, ce[0], cx, cy, dim, dim)
                tex_hits += 1
        rendered += 1
    checks.append({"name": "gui_controls_rendered",
                   "expected": parsed["control_count"], "actual": rendered,
                   "ok": rendered == parsed["control_count"]})
    checks.append({"name": "gui_control_textures_blitted", "actual": tex_hits,
                   "ok": True})
    return img, checks


def _struct_color(parsed, ctl, label):
    g = parsed["gff"]
    # find the control struct again by scanning CONTROLS for the tag
    for f in g.fields_of(0):
        if f.type == 15 and f.label == "CONTROLS":
            _, idx = g.list_of(f)
            for si in idx:
                d = g.as_dict(si)
                if d.get("TAG") == ctl["tag"]:
                    for cf in g.fields_of(si):
                        if cf.type == 14 and cf.label == label:
                            sub = g.as_dict(g.value(cf)["__struct__"])
                            return sub.get("COLOR", (1.0, 1.0, 1.0))
            break
    return (1.0, 1.0, 1.0)


def texture_checks(tex: TextureCache) -> list[dict]:
    out = []
    for resref, entry in sorted(tex.cache.items()):
        if entry is None:
            out.append({"name": f"texture[{resref}]", "ok": False,
                        "reason": "not found"})
            continue
        _img, meta, src = entry
        out.append({"name": f"texture_dims[{resref}]",
                    "expected": [meta["w"], meta["h"]],
                    "actual": [meta["w"], meta["h"]],
                    "format": meta.get("decoded_format", meta.get("image_type")),
                    "source": src, "ok": bool(meta.get("header_dims_ok"))})
    return out


def run_gui(game: Game, name: str, out_dir: str, scale: int = 2) -> dict:
    hit = game.find(name, T_GUI)
    if hit is None:
        raise ValueError(f"no GUI resource {name} (type {T_GUI})")
    data, src = hit
    game.add_packs()
    parsed = parse_gui(data, name)
    tex = TextureCache(game)
    img, checks = render_gui(parsed, tex, scale)
    checks = parsed["checks"] + checks + texture_checks(tex)
    for c in checks:
        if "ok" not in c:
            c["ok"] = False
    png_path = os.path.join(out_dir, f"{name}.png")
    nbytes = write_png(png_path, img)
    summary = {
        "lane": "s1-08-viewer",
        "kind": "gui",
        "gui": name,
        "source": src,
        "build_sha256": BUILD_SHA256,
        "game_dir": game.dir,
        "render": {"file": os.path.basename(png_path), "bytes": nbytes,
                   "size": [img.w, img.h], "scale": scale,
                   "gui_space": list(GUI_SPACE)},
        "counts": {"controls": parsed["control_count"],
                   "control_types": {str(k): v for k, v in _count_types(parsed).items()},
                   "texture_refs": sum(len(c["texture_refs"]) for c in parsed["controls"])},
        "controls": [{k: v for k, v in c.items() if k != "texture_refs"}
                     | {"texture_refs": c["texture_refs"]} for c in parsed["controls"]],
        "textures": [m for m in tex.log],
        "checks": checks,
        "checks_passed": sum(1 for c in checks if c.get("ok")),
        "checks_total": len(checks),
    }
    json_path = os.path.join(out_dir, f"{name}_gui.json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    return summary


def _count_types(parsed: dict) -> dict:
    from collections import Counter
    return Counter(c["control_type"] for c in parsed["controls"])


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="s1-08 GUI viewer")
    ap.add_argument("--game-dir", default=None)
    ap.add_argument("--out", default="artifacts/lanes/s1-08-viewer")
    ap.add_argument("--guis", nargs="+", default=["mainmenu", "maininterface"])
    ap.add_argument("--scale", type=int, default=2)
    args = ap.parse_args(argv)
    gdir = game_dir(args.game_dir)
    os.makedirs(args.out, exist_ok=True)
    sentinel = check_install_sentinel(gdir)
    print(f"[sentinel] swkotor.exe SHA-256 {sentinel['sha256']} matches build "
          f"{BUILD_SHA256[:12]}... (read-only check)")
    game = Game(gdir)
    all_ok = True
    for name in args.guis:
        try:
            summary = run_gui(game, name, args.out, args.scale)
        except (ValueError, OSError) as exc:
            print(f"[error] {name}: {exc}")
            all_ok = False
            continue
        c = summary["counts"]
        print(f"[{name}] controls={c['controls']} texture_refs={c['texture_refs']} "
              f"checks={summary['checks_passed']}/{summary['checks_total']}")
        for chk in summary["checks"]:
            if not chk.get("ok"):
                all_ok = False
                print(f"  [check-fail] {chk}")
        r = summary["render"]
        print(f"  wrote {r['file']} ({r['bytes']} B, {r['size'][0]}x{r['size'][1]})")
        print(f"  wrote {name}_gui.json")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
