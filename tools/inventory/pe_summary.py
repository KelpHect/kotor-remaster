#!/usr/bin/env python3
"""Minimal read-only PE32/PE32+ summary for provenance and RE planning.

Prints machine, image base, entry RVA, sections, and imported DLL names.
No external dependencies.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from datetime import datetime, timezone


def u16(d: bytes, o: int) -> int:
    return struct.unpack_from("<H", d, o)[0]


def u32(d: bytes, o: int) -> int:
    return struct.unpack_from("<I", d, o)[0]


def u64(d: bytes, o: int) -> int:
    return struct.unpack_from("<Q", d, o)[0]


def cstring(d: bytes, o: int) -> str:
    end = d.find(b"\x00", o)
    return d[o:end if end >= 0 else len(d)].decode("latin-1", "replace")


def rva_to_offset(sections: list[dict], rva: int) -> int | None:
    for s in sections:
        if s["va"] <= rva < s["va"] + max(s["vsize"], s["raw_size"]):
            return s["raw_ptr"] + (rva - s["va"])
    return None


def parse(path: str) -> dict:
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        raise ValueError("not an MZ/PE file")
    pe = u32(data, 0x3C)
    if data[pe:pe + 4] != b"PE\x00\x00":
        raise ValueError("missing PE signature")
    coff = pe + 4
    machine = u16(data, coff)
    nsec = u16(data, coff + 2)
    timestamp = u32(data, coff + 4)
    opt_size = u16(data, coff + 16)
    chars = u16(data, coff + 18)
    opt = coff + 20
    magic = u16(data, opt)
    is64 = magic == 0x20B
    entry_rva = u32(data, opt + 16)
    image_base = u64(data, opt + 24) if is64 else u32(data, opt + 28)
    size_image = u32(data, opt + 56)
    checksum = u32(data, opt + 64)
    dd = opt + (112 if is64 else 96)
    num_dirs = u32(data, dd - 4)
    sections = []
    sec = opt + opt_size
    for i in range(nsec):
        b = sec + i * 40
        name = data[b:b + 8].split(b"\x00")[0].decode("latin-1", "replace")
        sections.append({
            "name": name,
            "vsize": u32(data, b + 8),
            "va": u32(data, b + 12),
            "raw_size": u32(data, b + 16),
            "raw_ptr": u32(data, b + 20),
            "characteristics": u32(data, b + 36),
        })

    imports = []
    if num_dirs > 1:
        imp_rva, imp_size = struct.unpack_from("<II", data, dd + 8)
        off = rva_to_offset(sections, imp_rva) if imp_rva else None
        if off:
            for i in range(4096):
                desc = off + i * 20
                if desc + 20 > len(data) or data[desc:desc + 20] == b"\x00" * 20:
                    break
                name_rva = u32(data, desc + 12)
                noff = rva_to_offset(sections, name_rva)
                if noff:
                    imports.append(cstring(data, noff))

    rich_offset = data.rfind(b"Rich", 0, pe)
    return {
        "path": os.path.abspath(path),
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "md5": hashlib.md5(data).hexdigest(),
        "mtime_utc": datetime.fromtimestamp(os.stat(path).st_mtime, timezone.utc).isoformat(),
        "machine": hex(machine),
        "is_64bit": is64,
        "timestamp": timestamp,
        "timestamp_utc": datetime.fromtimestamp(timestamp, timezone.utc).isoformat() if 0 < timestamp < 2**32 else None,
        "image_base": hex(image_base),
        "entry_rva": hex(entry_rva),
        "size_of_image": size_image,
        "checksum": hex(checksum),
        "characteristics": hex(chars),
        "rich_header_present": rich_offset >= 0,
        "sections": sections,
        "import_dlls": imports,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    info = parse(args.path)
    if args.json:
        print(json.dumps(info, indent=2))
        return 0
    for key in ("path", "size", "sha256", "md5", "mtime_utc", "machine", "image_base",
                "entry_rva", "size_of_image", "checksum", "rich_header_present"):
        print(f"{key:>18}: {info[key]}")
    print("sections:")
    for s in info["sections"]:
        print(f"  {s['name']:<8} va=0x{s['va']:08x} vsize=0x{s['vsize']:<8x} raw=0x{s['raw_size']:<8x} flags=0x{s['characteristics']:08x}")
    print("imports:", ", ".join(info["import_dlls"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
