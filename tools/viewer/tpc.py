"""Minimal TPC and TGA decoders (stdlib only) for the s1-08 viewer.

TPC layout (verified against this install; docs/recon/02_TECHNICAL_NOTES.md
§2 and header probes of TexturePacks/*.erf + patch.erf, build SHA-256
0290889e...74e6):

* 128-byte header: u32 data_size @0x00 (top-mip bytes; 0 = raw), f32
  alpha_test @0x04, u16 width @0x08, u16 height @0x0A, u8 encoding @0x0C
  (1 gray, 2 RGB-or-DXT1, 4 RGBA-or-DXT5, 0x0C BGRA), u8 mip_count @0x0D.
* Mip 0 payload starts at byte 128; further mips follow (not decoded here);
  TXI text is appended after the last mip.
* Encoding 2/4 disambiguation by data_size: w*h*3 -> RGB, w*h*4 -> RGBA,
  otherwise DXT1 (enc 2) / DXT5 (enc 4).  data_size == 0 means raw pixels.

TGA (type 3, base-BIF textures/lightmaps): image types 1/2/3/9/10/11,
8/24/32 bpp, origin bit 0x20 (top-down) and 0x10 (right-to-left) honored.

Both decoders return (Image, meta-dict); meta always carries w/h/format so
the viewer can self-check decoded dimensions against the file header.
"""

from __future__ import annotations

import struct

from .pngwrite import Image

TPC_ENCODINGS = {1: "gray", 2: "rgb/dxt1", 4: "rgba/dxt5", 0x0C: "bgra"}


def _rgb565(c: int):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return ((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31)


def _dxt1_colors(d: bytes, off: int, always4: bool):
    c0 = struct.unpack_from("<H", d, off)[0]
    c1 = struct.unpack_from("<H", d, off + 2)[0]
    p0 = _rgb565(c0)
    p1 = _rgb565(c1)
    if c0 > c1 or always4:
        p2 = tuple((2 * p0[i] + p1[i]) // 3 for i in range(3))
        p3 = tuple((p0[i] + 2 * p1[i]) // 3 for i in range(3))
        alphas = (255, 255, 255, 255)
    else:
        p2 = tuple((p0[i] + p1[i]) // 2 for i in range(3))
        p3 = (0, 0, 0)
        alphas = (255, 255, 255, 0)
    return (p0, p1, p2, p3), alphas


def _decode_dxt(d: bytes, off: int, w: int, h: int, img: Image, mode: str):
    """Decode one DXT1/3/5 mip into img (RGBA). mode in {'1','3','5'}."""
    bw = (w + 3) // 4
    bh = (h + 3) // 4
    for by in range(bh):
        for bx in range(bw):
            bo = off + (by * bw + bx) * (8 if mode == "1" else 16)
            if mode == "1":
                colors, a4 = _dxt1_colors(d, bo, False)
                bits = struct.unpack_from("<I", d, bo + 4)[0]
                alphas = None  # per-pixel alpha derived from color index
            else:
                if mode == "3":
                    a0 = struct.unpack_from("<Q", d, bo)[0]
                    colors, _ = _dxt1_colors(d, bo + 8, True)
                    alphas = tuple(((a0 >> (4 * i)) & 0xF) * 17 for i in range(16))
                    bits = struct.unpack_from("<I", d, bo + 12)[0]
                else:  # DXT5
                    a0, a1 = d[bo], d[bo + 1]
                    abits = int.from_bytes(d[bo + 2:bo + 8], "little")
                    if a0 > a1:
                        tbl = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                    else:
                        tbl = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
                    alphas = tuple(tbl[(abits >> (3 * i)) & 7] for i in range(16))
                    colors, _ = _dxt1_colors(d, bo + 8, True)
                    bits = struct.unpack_from("<I", d, bo + 12)[0]
            for i in range(16):
                px = bx * 4 + (i & 3)
                py = by * 4 + (i >> 2)
                if px >= w or py >= h:
                    continue
                ci = (bits >> (2 * i)) & 3
                r, g, b = colors[ci]
                a = alphas[i] if alphas is not None else a4[ci]
                img.set(px, py, (r, g, b), a)


def decode_tpc(data: bytes) -> tuple[Image, dict]:
    """Decode the top mip of a TPC payload. Returns (Image RGBA, meta)."""
    if len(data) < 128:
        raise ValueError("TPC shorter than 128-byte header")
    data_size = struct.unpack_from("<I", data, 0)[0]
    alpha_test = struct.unpack_from("<f", data, 4)[0]
    w, h = struct.unpack_from("<2H", data, 8)
    enc = data[12]
    mips = data[13]
    if not (0 < w <= 4096 and 0 < h <= 4096):
        raise ValueError(f"TPC implausible dims {w}x{h}")
    img = Image(w, h, channels=4)
    dxt1_size = ((w + 3) // 4) * ((h + 3) // 4) * 8
    dxt5_size = ((w + 3) // 4) * ((h + 3) // 4) * 16
    fmt = None
    if enc == 1:
        fmt = "gray"
    elif enc == 2:
        fmt = "rgb" if data_size == w * h * 3 else "dxt1"
    elif enc == 4:
        if data_size == w * h * 4 or data_size == 0:
            fmt = "rgba"
        else:
            fmt = "dxt5"
    elif enc == 0x0C:
        fmt = "bgra"
    else:
        raise ValueError(f"TPC unknown encoding 0x{enc:02x}")
    off = 128
    if fmt == "gray":
        n = w * h
        for i in range(n):
            g = data[off + i] if off + i < len(data) else 0
            img.set(i % w, i // w, (g, g, g), 255)
    elif fmt == "rgb":
        for i in range(w * h):
            o = off + i * 3
            img.set(i % w, i // w, (data[o], data[o + 1], data[o + 2]), 255)
    elif fmt == "rgba":
        for i in range(w * h):
            o = off + i * 4
            img.set(i % w, i // w, (data[o], data[o + 1], data[o + 2]), data[o + 3])
    elif fmt == "bgra":
        for i in range(w * h):
            o = off + i * 4
            img.set(i % w, i // w, (data[o + 2], data[o + 1], data[o]), data[o + 3])
    elif fmt == "dxt1":
        _decode_dxt(data, off, w, h, img, "1")
    elif fmt == "dxt5":
        _decode_dxt(data, off, w, h, img, "5")
    else:  # pragma: no cover
        raise ValueError(fmt)
    meta = {
        "format": "TPC", "encoding": enc, "encoding_name": TPC_ENCODINGS.get(enc, hex(enc)),
        "decoded_format": fmt, "w": w, "h": h, "mips": mips,
        "data_size": data_size, "alpha_test": alpha_test,
        "payload_bytes": len(data),
        "header_dims_ok": (img.w == w and img.h == h),
        "cubemap": fmt in ("dxt1", "dxt5") and data_size not in (dxt1_size, dxt5_size)
                   and abs(data_size - (dxt1_size if fmt == "dxt1" else dxt5_size)) > 0
                   and data_size * 6 in (dxt1_size, dxt5_size),
    }
    return img, meta


def decode_tga(data: bytes) -> tuple[Image, dict]:
    """Decode a TGA payload (types 1/2/3, RLE variants 9/10/11)."""
    if len(data) < 18:
        raise ValueError("TGA shorter than 18-byte header")
    idlen = data[0]
    cmap_type = data[1]
    img_type = data[2]
    w, h = struct.unpack_from("<2H", data, 12)
    bpp = data[16]
    desc = data[17]
    if img_type not in (1, 2, 3, 9, 10, 11):
        raise ValueError(f"TGA unsupported image type {img_type}")
    if bpp not in (8, 24, 32):
        raise ValueError(f"TGA unsupported bpp {bpp}")
    rle = img_type >= 9
    base = img_type - 8 if rle else img_type  # 9/10/11 -> 1 cmap, 2 rgb, 3 gray
    cmap_len = 0
    cmap_entry = 0
    if cmap_type == 1:
        cmap_len = struct.unpack_from("<H", data, 5)[0]
        cmap_entry = struct.unpack_from("<H", data, 7)[0] // 8
        if cmap_entry not in (16, 24, 32):
            raise ValueError("TGA cmap entry size unsupported")
    pos = 18 + idlen
    cmap = b""
    if cmap_type == 1:
        cmap = data[pos:pos + cmap_len * (cmap_entry // 8)]
        pos += cmap_len * (cmap_entry // 8)
    img = Image(w, h, channels=4)
    bypp = bpp // 8
    top_down = bool(desc & 0x20)
    right_left = bool(desc & 0x10)

    def put(x, y, rgb, a):
        if right_left:
            x = w - 1 - x
        if not top_down:
            y = h - 1 - y
        img.set(x, y, rgb, a)

    def read_pixel():
        nonlocal pos
        if base == 3 or (base == 1 and cmap_entry == 8):
            g = data[pos]; pos += 1
            return (g, g, g), 255
        if base == 1:
            idx = struct.unpack_from("<H" if cmap_entry > 8 else "<B", data, pos)[0]
            pos += cmap_entry // 8
            o = idx * (cmap_entry // 8)
            if cmap_entry == 16:
                c = struct.unpack_from("<H", cmap, o)[0]
                return _rgb565(c), 255
            return (cmap[o + 2], cmap[o + 1], cmap[o]), (cmap[o + 3] if cmap_entry == 32 else 255)
        o = pos
        pos += bypp
        if bpp == 24:
            return (data[o + 2], data[o + 1], data[o]), 255
        return (data[o + 2], data[o + 1], data[o]), data[o + 3]

    total = w * h
    written = 0
    while written < total:
        if rle:
            hdr = data[pos]; pos += 1
            count = (hdr & 0x7F) + 1
            if hdr & 0x80:
                rgb, a = read_pixel()
                for _ in range(count):
                    put(written % w, written // w, rgb, a)
                    written += 1
            else:
                for _ in range(count):
                    rgb, a = read_pixel()
                    put(written % w, written // w, rgb, a)
                    written += 1
        else:
            rgb, a = read_pixel()
            put(written % w, written // w, rgb, a)
            written += 1
    meta = {
        "format": "TGA", "image_type": img_type, "bpp": bpp, "w": w, "h": h,
        "top_down": top_down, "right_left": right_left,
        "header_dims_ok": (img.w == w and img.h == h),
    }
    return img, meta


def decode_texture(data: bytes, res_type: int | None = None) -> tuple[Image, dict]:
    """Decode a texture payload. res_type 3007 -> TPC, 3 -> TGA; otherwise sniff."""
    if res_type == 3007:
        return decode_tpc(data)
    if res_type == 3:
        return decode_tga(data)
    if len(data) >= 128:
        w, h = struct.unpack_from("<2H", data, 8)
        if data[12] in TPC_ENCODINGS and 0 < w <= 4096 and 0 < h <= 4096:
            try:
                return decode_tpc(data)
            except ValueError:
                pass
    return decode_tga(data)
