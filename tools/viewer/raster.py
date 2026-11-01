"""Tiny software rasterizer: flat and affine-textured triangles with a
z-buffer, rect fills, stretched blits and lines.  Pure Python; tuned to keep
per-pixel work in local variables (this is the lane's only hot loop).

Coordinates: screen space, y down, pixel centers at integer coords.
"""

from __future__ import annotations

from .pngwrite import Image


def new_zbuf(w: int, h: int) -> list[float]:
    return [1.0e30] * (w * h)


def fill_rect(img: Image, z: list[float] | None, x0: int, y0: int,
              x1: int, y1: int, rgb, depth: float = 0.0, a: int = 255):
    x0 = max(0, int(x0)); y0 = max(0, int(y0))
    x1 = min(img.w, int(x1)); y1 = min(img.h, int(y1))
    if x1 <= x0 or y1 <= y0:
        return
    c = img.channels
    r, g, b = rgb[0] & 0xFF, rgb[1] & 0xFF, rgb[2] & 0xFF
    for y in range(y0, y1):
        base = (y * img.w + x0) * c
        for x in range(x0, x1):
            i = base + (x - x0) * c
            if z is not None and depth >= z[y * img.w + x]:
                continue
            if a >= 255:
                img.pix[i] = r; img.pix[i + 1] = g; img.pix[i + 2] = b
                if c == 4:
                    img.pix[i + 3] = 255
            elif a > 0:
                ia = 255 - a
                img.pix[i] = (r * a + img.pix[i] * ia) // 255
                img.pix[i + 1] = (g * a + img.pix[i + 1] * ia) // 255
                img.pix[i + 2] = (b * a + img.pix[i + 2] * ia) // 255
                if c == 4:
                    img.pix[i + 3] = min(255, a + img.pix[i + 3] * ia // 255)
        if z is not None:
            for x in range(x0, x1):
                if depth < z[y * img.w + x]:
                    z[y * img.w + x] = depth


def _tri_setup(p):
    # sort by y
    if p[1][1] < p[0][1]:
        p[0], p[1] = p[1], p[0]
    if p[2][1] < p[1][1]:
        p[1], p[2] = p[2], p[1]
    if p[1][1] < p[0][1]:
        p[0], p[1] = p[1], p[0]
    return p


def tri_flat(img: Image, z: list[float], p, rgb) -> int:
    """Rasterize a flat triangle; p = [(x,y,depth), ...]. Returns pixel count."""
    p = _tri_setup([list(q) for q in p])
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = p
    minx = max(0, int(min(x0, x1, x2)))
    maxx = min(img.w - 1, int(max(x0, x1, x2)) + 1)
    miny = max(0, int(min(y0, y1, y2)))
    maxy = min(img.h - 1, int(max(y0, y1, y2)) + 1)
    if minx > maxx or miny > maxy:
        return 0
    d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
    if d == 0:
        return 0
    inv = 1.0 / d
    c = img.channels
    r, g, b = rgb[0] & 0xFF, rgb[1] & 0xFF, rgb[2] & 0xFF
    pix = img.pix
    w = img.w
    n = 0
    for y in range(miny, maxy + 1):
        yc = y + 0.5
        row = y * w
        for x in range(minx, maxx + 1):
            xc = x + 0.5
            a = ((y1 - y2) * (xc - x2) + (x2 - x1) * (yc - y2)) * inv
            bb = ((y2 - y0) * (xc - x2) + (x0 - x2) * (yc - y2)) * inv
            cc = 1.0 - a - bb
            if a < -1e-9 or bb < -1e-9 or cc < -1e-9:
                continue
            zi = a * z0 + bb * z1 + cc * z2
            idx = row + x
            if zi >= z[idx]:
                continue
            z[idx] = zi
            i = idx * c
            pix[i] = r; pix[i + 1] = g; pix[i + 2] = b
            if c == 4:
                pix[i + 3] = 255
            n += 1
    return n


def tri_tex(img: Image, z: list[float], p, uv, tex: Image) -> int:
    """Affine-textured triangle with z-buffer and per-pixel alpha.

    p = [(x, y, depth)], uv = [(u, v)] in texel units; tex is an Image with
    channels 3 (opaque) or 4 (alpha).
    Returns pixel count.
    """
    p = _tri_setup([list(q) for q in p])
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = p
    (u0, v0), (u1, v1), (u2, v2) = uv
    minx = max(0, int(min(x0, x1, x2)))
    maxx = min(img.w - 1, int(max(x0, x1, x2)) + 1)
    miny = max(0, int(min(y0, y1, y2)))
    maxy = min(img.h - 1, int(max(y0, y1, y2)) + 1)
    if minx > maxx or miny > maxy:
        return 0
    d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
    if d == 0:
        return 0
    inv = 1.0 / d
    tc = tex.channels
    tp = tex.pix
    tw = tex.w
    th = tex.h
    c = img.channels
    pix = img.pix
    w = img.w
    n = 0
    for y in range(miny, maxy + 1):
        yc = y + 0.5
        row = y * w
        for x in range(minx, maxx + 1):
            xc = x + 0.5
            a = ((y1 - y2) * (xc - x2) + (x2 - x1) * (yc - y2)) * inv
            bb = ((y2 - y0) * (xc - x2) + (x0 - x2) * (yc - y2)) * inv
            cc = 1.0 - a - bb
            if a < -1e-9 or bb < -1e-9 or cc < -1e-9:
                continue
            zi = a * z0 + bb * z1 + cc * z2
            idx = row + x
            if zi >= z[idx]:
                continue
            z[idx] = zi
            u = a * u0 + bb * u1 + cc * u2
            v = a * v0 + bb * v1 + cc * v2
            tx = int(u) % tw
            ty = int(v) % th
            ti = (ty * tw + tx) * tc
            alpha = tp[ti + 3] if tc == 4 else 255
            if alpha == 0:
                continue
            i = idx * c
            if alpha >= 255:
                pix[i] = tp[ti]; pix[i + 1] = tp[ti + 1]; pix[i + 2] = tp[ti + 2]
                if c == 4:
                    pix[i + 3] = 255
            else:
                ia = 255 - alpha
                pix[i] = (tp[ti] * alpha + pix[i] * ia) // 255
                pix[i + 1] = (tp[ti + 1] * alpha + pix[i + 1] * ia) // 255
                pix[i + 2] = (tp[ti + 2] * alpha + pix[i + 2] * ia) // 255
                if c == 4:
                    pix[i + 3] = min(255, alpha + pix[i + 3] * ia // 255)
            n += 1
    return n


def blit_stretch(img: Image, tex: Image, x0: int, y0: int, w: int, h: int) -> int:
    """Nearest-neighbour stretch of tex into the rect (alpha-blended)."""
    if w <= 0 or h <= 0:
        return 0
    x0 = max(0, x0); y0 = max(0, y0)
    x1 = min(img.w, x0 + w); y1 = min(img.h, y0 + h)
    if x1 <= x0 or y1 <= y0:
        return 0
    tc = tex.channels
    tp = tex.pix
    tw = tex.w
    th = tex.h
    c = img.channels
    pix = img.pix
    iw = img.w
    n = 0
    for y in range(y0, y1):
        ty = (y - y0) * th // max(1, (y1 - y0))
        if ty >= th:
            ty = th - 1
        for x in range(x0, x1):
            tx = (x - x0) * tw // max(1, (x1 - x0))
            if tx >= tw:
                tx = tw - 1
            ti = (ty * tw + tx) * tc
            alpha = tp[ti + 3] if tc == 4 else 255
            if alpha == 0:
                continue
            i = (y * iw + x) * c
            if alpha >= 255:
                pix[i] = tp[ti]; pix[i + 1] = tp[ti + 1]; pix[i + 2] = tp[ti + 2]
                if c == 4:
                    pix[i + 3] = 255
            else:
                ia = 255 - alpha
                pix[i] = (tp[ti] * alpha + pix[i] * ia) // 255
                pix[i + 1] = (tp[ti + 1] * alpha + pix[i + 1] * ia) // 255
                pix[i + 2] = (tp[ti + 2] * alpha + pix[i + 2] * ia) // 255
                if c == 4:
                    pix[i + 3] = min(255, alpha + pix[i + 3] * ia // 255)
            n += 1
    return n


def draw_line(img: Image, x0: int, y0: int, x1: int, y1: int, rgb, a: int = 255):
    """Bresenham line."""
    x0, y0, x1, y1 = int(x0), int(y0), int(x1), int(y1)
    dx = abs(x1 - x0)
    dy = -abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    while True:
        img.blend(x0, y0, rgb, a)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x0 += sx
        if e2 <= dx:
            err += dx
            y0 += sy
