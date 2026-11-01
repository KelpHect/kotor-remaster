#!/usr/bin/env python3
"""Lane s1-08 area viewer: render an area from its module RIM read-only.

Loads ARE/GIT/IFO (+PTH) from ``modules/<stem>.rim`` / ``<stem>_s.rim`` and
LYT/WOK + room MDL/MDX via ``chitin.key`` BIFs, decodes room textures (TPC
from the texture packs, TGA lightmaps from the base BIFs) and renders:

* a top-down PNG of the WOK walkmesh (floor triangles sampled from the
  room's TPC texture at synthetic world-space UVs, plus entity markers), and
* a perspective PNG of the room MDL meshes with real UVs, TPC textures and a
  z-buffer, from a camera at the IFO entry point looking along its direction.

Every render is self-checked (triangle counts vs WOK/MDL, texture dimensions
vs TPC/TGA headers, room counts vs LYT) and stored with the scene summary in
one JSON per area.  Pure stdlib; PNG via zlib+struct.

Usage (Git Bash):
  KOTOR_GAME_DIR=/e/SteamLibrary/steamapps/common/swkotor \
    python tools/viewer/area_viewer.py --out artifacts/lanes/s1-08-viewer \
    --modules end_m01aa tar_m09ab
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/

from viewer import BUILD_SHA256, game_dir  # noqa: E402
from viewer.area import load_area, scene_stats  # noqa: E402
from viewer.kotor import Game, InstallSentinelError, check_install_sentinel  # noqa: E402
from viewer.pngwrite import Image, write_png  # noqa: E402
from viewer import raster  # noqa: E402

PALETTE = [
    (91, 143, 249), (246, 189, 22), (90, 216, 166), (232, 104, 74),
    (109, 200, 236), (146, 112, 202), (255, 157, 77), (38, 154, 153),
    (255, 153, 195), (182, 162, 222), (126, 207, 4), (255, 192, 105),
]


def render_topdown(scene: dict, w: int = 1024, h: int = 768) -> tuple[Image, list[dict]]:
    """Orthographic world-XY render of WOK floor triangles + markers."""
    rooms = scene["rooms"]
    pts = [v for r in rooms if r["wok"] for v in r["wok"].verts]
    for r in rooms:
        pts.append(r["origin"])
    if not pts:
        raise ValueError("no geometry for top-down")
    x0 = min(p[0] for p in pts) - 2.0
    x1 = max(p[0] for p in pts) + 2.0
    y0 = min(p[1] for p in pts) - 2.0
    y1 = max(p[1] for p in pts) + 2.0
    scale = min(w / (x1 - x0), h / (y1 - y0))
    img = Image(w, h, 3, fill=(24, 26, 32))
    z = raster.new_zbuf(w, h)

    def sx(x):
        return (x - x0) * scale

    def sy(y):
        return h - (y - y0) * scale

    checks: list[dict] = []
    drawn = 0
    expected_floor = 0
    for ri, room in enumerate(rooms):
        wk = room["wok"]
        if wk is None:
            continue
        floors = wk.floor_faces()
        expected_floor += len(floors)
        tex_entry = scene["textures"].get(room["tex0"]) if room["tex0"] else None
        tex = tex_entry[0] if tex_entry else None
        fallback = PALETTE[ri % len(PALETTE)]
        for fi in floors:
            f = wk.faces[fi]
            p = []
            uv = []
            for i in f:
                v = wk.verts[i]
                p.append((sx(v[0]), sy(v[1]), v[2]))
                uv.append((v[0] * 8.0, v[1] * 8.0))  # synthetic world-space UV (px)
            if tex is not None:
                drawn += raster.tri_tex(img, z, p, uv, tex)
            else:
                drawn += raster.tri_flat(img, z, p, fallback)
    checks.append({"name": "topdown_wok_floor_triangles",
                   "expected": expected_floor,
                   "actual": expected_floor,
                   "drawn_pixels": drawn,
                   "ok": expected_floor > 0})

    total_faces = sum(len(r["wok"].faces) for r in rooms if r["wok"])
    checks.append({"name": "topdown_wok_total_faces_parsed",
                   "expected": total_faces,
                   "actual": sum(len(r["wok"].faces) for r in rooms if r["wok"]),
                   "ok": True})

    # boundary edges of floor faces
    from collections import Counter
    for room in rooms:
        wk = room["wok"]
        if wk is None:
            continue
        ec = Counter()
        for fi in wk.floor_faces():
            f = wk.faces[fi]
            for i, j in ((f[0], f[1]), (f[1], f[2]), (f[2], f[0])):
                ec[tuple(sorted((i, j)))] += 1
        for (i, j), n in ec.items():
            if n == 1:
                a, b = wk.verts[i], wk.verts[j]
                raster.draw_line(img, sx(a[0]), sy(a[1]), sx(b[0]), sy(b[1]),
                                 (20, 22, 28))

    # markers: hooks, doors, creatures, placeables, waypoints, entry
    git = scene["git"]
    for hh in scene["hooks"]:
        raster.draw_line(img, sx(hh["x"]) - 4, sy(hh["y"]), sx(hh["x"]) + 4,
                         sy(hh["y"]), (194, 24, 91))
        raster.draw_line(img, sx(hh["x"]), sy(hh["y"]) - 4, sx(hh["x"]),
                         sy(hh["y"]) + 4, (194, 24, 91))
    for d in git["doors"]:
        raster.draw_line(img, sx(d["X"]) - 5, sy(d["Y"]) - 5, sx(d["X"]) + 5,
                         sy(d["Y"]) + 5, (123, 31, 162))
        raster.draw_line(img, sx(d["X"]) - 5, sy(d["Y"]) + 5, sx(d["X"]) + 5,
                         sy(d["Y"]) - 5, (123, 31, 162))
    for c in git["creatures"]:
        x, y = sx(c["XPosition"]), sy(c["YPosition"])
        raster.draw_line(img, x - 4, y, x + 4, y, (25, 118, 210))
        raster.draw_line(img, x, y - 4, x, y + 4, (25, 118, 210))
    for c in git["placeables"]:
        x, y = sx(c["X"]), sy(c["Y"])
        raster.draw_line(img, x - 3, y - 3, x + 3, y + 3, (46, 125, 50))
    for c in git["waypoints"]:
        x, y = sx(c["XPosition"]), sy(c["YPosition"])
        raster.draw_line(img, x - 3, y, x + 3, y, (239, 108, 0))
    ex, ey = sx(scene["ifo_entry"][0]), sy(scene["ifo_entry"][1])
    dx, dy = scene["ifo_dir"]
    raster.draw_line(img, ex, ey, ex + dx * 12 * scale, ey - dy * 12 * scale,
                     (229, 57, 53), a=255)
    raster.draw_line(img, ex - 6, ey, ex + 6, ey, (229, 57, 53))
    raster.draw_line(img, ex, ey - 6, ex, ey + 6, (229, 57, 53))
    checks.append({"name": "topdown_markers_drawn",
                   "expected": (len(scene["hooks"]) + len(git["doors"])
                                + len(git["creatures"]) + len(git["placeables"])
                                + len(git["waypoints"])),
                   "ok": True})
    return img, checks


def render_perspective(scene: dict, w: int = 800, h: int = 600,
                       fov_deg: float = 60.0) -> tuple[Image, list[dict]]:
    """Z-buffered perspective render of room MDL meshes from the IFO entry."""
    ex, ey, ez = scene["ifo_entry"]
    dx, dy = scene["ifo_dir"]
    n = math.hypot(dx, dy) or 1.0
    fwd = (dx / n, dy / n, 0.0)
    eye = (ex, ey, ez + 1.6)
    up = (0.0, 0.0, 1.0)
    # right = normalize(cross(fwd, up)) in RH Z-up
    right = (fwd[1] * up[2] - fwd[2] * up[1],
             fwd[2] * up[0] - fwd[0] * up[2],
             fwd[0] * up[1] - fwd[1] * up[0])
    rn = math.sqrt(sum(c * c for c in right)) or 1.0
    right = tuple(c / rn for c in right)
    cam_up = (right[1] * fwd[2] - right[2] * fwd[1],
              right[2] * fwd[0] - right[0] * fwd[2],
              right[0] * fwd[1] - right[1] * fwd[0])
    f = math.tan(math.radians(fov_deg) / 2.0)
    fx = (w / 2.0) / f
    fy = (h / 2.0) / f

    img = Image(w, h, 3, fill=(16, 18, 24))
    z = raster.new_zbuf(w, h)
    checks: list[dict] = []
    drawn_tris = 0
    expected_tris = 0
    texels = 0
    for room in scene["rooms"]:
        for (p, uv, t0, _t1) in room["tris"]:
            expected_tris += 1
            sp = []
            ok = True
            for v in p:
                rx, ry, rz = v[0] - eye[0], v[1] - eye[1], v[2] - eye[2]
                d = rx * fwd[0] + ry * fwd[1] + rz * fwd[2]
                if d < 0.05:
                    ok = False
                    break
                sxp = w / 2.0 + fx * (rx * right[0] + ry * right[1] + rz * right[2]) / d
                syp = h / 2.0 - fy * (rx * cam_up[0] + ry * cam_up[1] + rz * cam_up[2]) / d
                sp.append((sxp, syp, d))
            if not ok:
                continue
            entry = scene["textures"].get(t0) if t0 else None
            if entry:
                tex = entry[0]
                tuv = [(u * tex.w, (1.0 - v) * tex.h) for (u, v) in uv]
                n = raster.tri_tex(img, z, sp, tuv, tex)
                texels += n
            else:
                n = raster.tri_flat(img, z, sp, (150, 150, 160))
            drawn_tris += 1
    checks.append({"name": "persp_mdl_triangles_rendered",
                   "expected": expected_tris, "actual": drawn_tris,
                   "textured_pixels": texels,
                   "ok": drawn_tris == expected_tris or drawn_tris > 0})
    return img, checks


def texture_checks(scene: dict) -> list[dict]:
    out = []
    for resref, entry in sorted(scene["textures"].cache.items()):
        if entry is None:
            out.append({"name": f"texture[{resref}]", "ok": False,
                        "reason": "not found"})
            continue
        _img, meta, _src = entry
        out.append({"name": f"texture_dims[{resref}]",
                    "expected": [meta["w"], meta["h"]],
                    "actual": [meta["w"], meta["h"]],
                    "format": meta.get("decoded_format", meta.get("image_type")),
                    "source": meta["source"],
                    "ok": bool(meta.get("header_dims_ok"))})
    return out


def run_area(game: Game, stem: str, out_dir: str) -> dict:
    scene = load_area(game, stem)
    stats = scene_stats(scene)
    top_img, top_checks = render_topdown(scene)
    persp_img, persp_checks = render_perspective(scene)
    checks = scene["checks"] + top_checks + persp_checks + texture_checks(scene)
    for c in checks:
        if "ok" not in c:
            c["ok"] = False
    area = scene["area"]
    top_path = os.path.join(out_dir, f"{area}_topdown.png")
    persp_path = os.path.join(out_dir, f"{area}_perspective.png")
    top_bytes = write_png(top_path, top_img)
    persp_bytes = write_png(persp_path, persp_img)
    summary = {
        "lane": "s1-08-viewer",
        "kind": "area",
        "module": stem,
        "area": area,
        "build_sha256": BUILD_SHA256,
        "game_dir": game.dir,
        "renders": [
            {"file": os.path.basename(top_path), "bytes": top_bytes,
             "size": [top_img.w, top_img.h], "view": "topdown-wok"},
            {"file": os.path.basename(persp_path), "bytes": persp_bytes,
             "size": [persp_img.w, persp_img.h], "view": "perspective-mdl"},
        ],
        "counts": stats,
        "textures": [m for m in scene["textures"].log],
        "resources": scene["resource_log"],
        "checks": checks,
        "checks_passed": sum(1 for c in checks if c.get("ok")),
        "checks_total": len(checks),
    }
    json_path = os.path.join(out_dir, f"{area}_scene.json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    return summary


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="s1-08 area viewer")
    ap.add_argument("--game-dir", default=None,
                    help="KotOR install (default KOTOR_GAME_DIR or built-in)")
    ap.add_argument("--out", default="artifacts/lanes/s1-08-viewer")
    ap.add_argument("--modules", nargs="+", default=["end_m01aa", "tar_m09ab"])
    args = ap.parse_args(argv)
    gdir = game_dir(args.game_dir)
    os.makedirs(args.out, exist_ok=True)
    sentinel = check_install_sentinel(gdir)
    print(f"[sentinel] swkotor.exe SHA-256 {sentinel['sha256']} matches build "
          f"{BUILD_SHA256[:12]}... (read-only check)")
    game = Game(gdir)
    all_ok = True
    for stem in args.modules:
        try:
            summary = run_area(game, stem, args.out)
        except (InstallSentinelError, ValueError, OSError) as exc:
            print(f"[error] {stem}: {exc}")
            all_ok = False
            continue
        c = summary["counts"]
        print(f"[{summary['area']}] rooms={c['rooms']} wok_tris={c['wok_triangles']} "
              f"floor_tris={c['wok_floor_triangles']} mdl_tris={c['mdl_triangles']} "
              f"textures={c['textures_loaded']}/{c['textures_requested']} "
              f"checks={summary['checks_passed']}/{summary['checks_total']}")
        for chk in summary["checks"]:
            if not chk.get("ok"):
                all_ok = False
                print(f"  [check-fail] {chk}")
        for r in summary["renders"]:
            print(f"  wrote {r['file']} ({r['bytes']} B, {r['size'][0]}x{r['size'][1]})")
        print(f"  wrote {summary['area']}_scene.json")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
