#!/usr/bin/env python3
"""Apply or revert the clean-room KotOR 1.0.3.0 1280x720 windowed patch.

The patch adds three trampolines in .text padding:
  1. FUN_005f0c60 accepts 1280x720 alongside the five shipped modes.
  2. FUN_006277c0 selects tooltip12X9 for 1280x720 instead of falling into
     the uninitialized-name path that crashes the resource manager.
  3. FUN_0068c100 selects the 1280 HUD resources for 1280x720.

Required [Graphics Options] settings in swkotor.ini:
  FullScreen=0, AllowWindowedMode=1, Width=1280, Height=720, RefreshRate=60,
  Frame Buffer=0, Shadows=0, Soft Shadows=0, Grass=0, Anti Aliasing=0

Frame Buffer=0 is required on modern systems: with Frame Buffer Effects enabled,
the load transition enters KotOR's offscreen/pbuffer path, the context switch
fails under current drivers, and the game black-screens after loading a save.

Usage:
  python tools/mod/kotor_windowed_1280.py --exe <swkotor.exe> --apply
  python tools/mod/kotor_windowed_1280.py --exe <swkotor.exe> --revert
  python tools/mod/kotor_windowed_1280.py --exe <swkotor.exe> --check
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

ORIGINAL_SHA256 = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6"
IMAGE_BASE = 0x00400000
TEXT_VS_NEW = 0x33C000

VALIDATOR_RVA = 0x1F0C60
VALIDATOR_ORIG = bytes.fromhex("8b4424043d")
VALIDATOR_RESUME_RVA = 0x1F0C69
VALIDATOR_CAVE_RVA = 0x33C1D0

TOOLTIP_CMP_RVA = 0x22783E
TOOLTIP_CMP_ORIG = bytes.fromhex("3dc0030000")
TOOLTIP_CAVE_RVA = 0x33C210
TOOLTIP_12X9_RVA = 0x227845
TOOLTIP_12X10_RVA = 0x227861
TOOLTIP_FALLBACK_RVA = 0x2278BA
TOOLTIP_CAVE_END = TOOLTIP_CAVE_RVA + 0x26

HUD_BRANCH_RVA = 0x28C52D
HUD_BRANCH_ORIG = bytes.fromhex("2dc0030000")
HUD_CAVE_RVA = 0x33C250
HUD_12X9_RVA = 0x28C54E
HUD_12X10_RVA = 0x28C539
HUD_FALLBACK_RVA = 0x28C57D
CAVE_AREA_END = 0x33C280

PATCH_MAGIC = b"KOTORW1280"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def rel32(next_rva: int, target_rva: int) -> bytes:
    return struct.pack("<i", target_rva - next_rva)


class PE:
    def __init__(self, data: bytes):
        self.data = bytearray(data)
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE image")
        self.pe = pe
        self.nsec = struct.unpack_from("<H", self.data, pe + 6)[0]
        self.opt = pe + 24
        if struct.unpack_from("<H", self.data, self.opt)[0] != 0x10B:
            raise ValueError("not PE32")
        opt_size = struct.unpack_from("<H", self.data, pe + 20)[0]
        self.sections = []
        for i in range(self.nsec):
            hdr = self.opt + opt_size + i * 40
            name = self.data[hdr:hdr + 8].rstrip(b"\0").decode("latin-1")
            vs, va, rs, ra = struct.unpack_from("<IIII", self.data, hdr + 8)
            self.sections.append({"name": name, "hdr": hdr, "vs": vs, "va": va,
                                  "rs": rs, "ra": ra})

    def section(self, name: str) -> dict:
        for s in self.sections:
            if s["name"] == name:
                return s
        raise ValueError(f"missing section {name}")

    def rva_to_off(self, rva: int) -> int:
        for s in self.sections:
            if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]):
                return s["ra"] + (rva - s["va"])
        raise ValueError(f"RVA 0x{rva:x} is outside all sections")

    def set_text_vs(self, value: int) -> None:
        t = self.section(".text")
        struct.pack_into("<I", self.data, t["hdr"] + 8, value)
        t["vs"] = value

    def get_bytes(self, rva: int, n: int) -> bytes:
        o = self.rva_to_off(rva)
        return bytes(self.data[o:o + n])

    def put_bytes(self, rva: int, blob: bytes) -> None:
        o = self.rva_to_off(rva)
        self.data[o:o + len(blob)] = blob


def build_validator_cave() -> bytes:
    c = bytearray()
    c += b"\x81\x7c\x24\x04" + struct.pack("<I", 1280)   # cmp dword [esp+4],1280
    c += b"\x75\x12"                                     # jne orig
    c += b"\x81\x7c\x24\x08" + struct.pack("<I", 720)    # cmp dword [esp+8],720
    c += b"\x75\x08"                                     # jne orig
    c += b"\xb8\x01\x00\x00\x00"                         # mov eax,1
    c += b"\xc2\x08\x00"                                 # ret 8
    c += bytes.fromhex("8b4424043d20030000")             # original: mov eax,[esp+4]; cmp eax,800
    c += b"\xe9" + rel32(VALIDATOR_CAVE_RVA + len(c) + 5, VALIDATOR_RESUME_RVA)
    return bytes(c)


def build_hud_cave() -> bytes:
    c = bytearray()
    c += bytes.fromhex("3d") + struct.pack("<I", 720)
    c += bytes.fromhex("0f84") + rel32(HUD_CAVE_RVA + len(c) + 6, HUD_12X9_RVA)
    c += bytes.fromhex("3d") + struct.pack("<I", 960)
    c += bytes.fromhex("0f84") + rel32(HUD_CAVE_RVA + len(c) + 6, HUD_12X9_RVA)
    c += bytes.fromhex("3d") + struct.pack("<I", 1024)
    c += bytes.fromhex("0f84") + rel32(HUD_CAVE_RVA + len(c) + 6, HUD_12X10_RVA)
    c += bytes.fromhex("e9") + rel32(HUD_CAVE_RVA + len(c) + 5, HUD_FALLBACK_RVA)
    return bytes(c)


def build_tooltip_cave() -> bytes:
    c = bytearray()
    c += b"\x3d" + struct.pack("<I", 720)                # cmp eax,720
    c += b"\x0f\x84" + rel32(TOOLTIP_CAVE_RVA + len(c) + 6, TOOLTIP_12X9_RVA)
    c += b"\x3d" + struct.pack("<I", 960)                # cmp eax,960
    c += b"\x0f\x84" + rel32(TOOLTIP_CAVE_RVA + len(c) + 6, TOOLTIP_12X9_RVA)
    c += b"\x3d" + struct.pack("<I", 1024)               # cmp eax,1024
    c += b"\x0f\x84" + rel32(TOOLTIP_CAVE_RVA + len(c) + 6, TOOLTIP_12X10_RVA)
    c += b"\xe9" + rel32(TOOLTIP_CAVE_RVA + len(c) + 5, TOOLTIP_FALLBACK_RVA)
    return bytes(c)


def validate_layout(pe: PE) -> None:
    if pe.get_bytes(VALIDATOR_RVA, len(VALIDATOR_ORIG)) != VALIDATOR_ORIG:
        raise SystemExit("validator bytes do not match 1.0.3.0")
    if pe.get_bytes(TOOLTIP_CMP_RVA, len(TOOLTIP_CMP_ORIG)) != TOOLTIP_CMP_ORIG:
        raise SystemExit("tooltip bytes do not match 1.0.3.0")
    if pe.get_bytes(HUD_BRANCH_RVA, len(HUD_BRANCH_ORIG)) != HUD_BRANCH_ORIG:
        raise SystemExit("HUD bytes do not match 1.0.3.0")
    cave = pe.get_bytes(VALIDATOR_CAVE_RVA, CAVE_AREA_END - VALIDATOR_CAVE_RVA)
    if any(b not in (0x00, 0xCC) for b in cave):
        raise SystemExit("trampoline area is not empty padding")


def apply(path: Path) -> dict:
    original = path.read_bytes()
    digest = sha256(original)
    if digest != ORIGINAL_SHA256:
        raise SystemExit(f"unexpected swkotor.exe SHA-256: {digest}")
    pe = PE(original)
    validate_layout(pe)
    text = pe.section(".text")
    if text["va"] != 0x1000:
        raise SystemExit("unexpected .text RVA")
    cave1 = build_validator_cave()
    cave2 = build_tooltip_cave()
    cave3 = build_hud_cave()
    if VALIDATOR_CAVE_RVA + len(cave1) > TOOLTIP_CAVE_RVA:
        raise SystemExit("validator cave overlaps tooltip cave")
    if TOOLTIP_CAVE_RVA + len(cave2) > HUD_CAVE_RVA:
        raise SystemExit("tooltip cave overlaps HUD cave")
    if HUD_CAVE_RVA + len(cave3) > CAVE_AREA_END:
        raise SystemExit("HUD cave exceeds reserved area")

    pe.set_text_vs(TEXT_VS_NEW)
    pe.put_bytes(VALIDATOR_CAVE_RVA, cave1)
    pe.put_bytes(TOOLTIP_CAVE_RVA, cave2)
    pe.put_bytes(HUD_CAVE_RVA, cave3)
    pe.put_bytes(VALIDATOR_RVA, b"\xe9" + rel32(VALIDATOR_RVA + 5, VALIDATOR_CAVE_RVA))
    pe.put_bytes(TOOLTIP_CMP_RVA, b"\xe9" + rel32(TOOLTIP_CMP_RVA + 5, TOOLTIP_CAVE_RVA))
    pe.put_bytes(HUD_BRANCH_RVA, b"\xe9" + rel32(HUD_BRANCH_RVA + 5, HUD_CAVE_RVA))

    patched = bytes(pe.data)
    backup = path.with_name(path.name + ".orig-1.0.3.0")
    if not backup.exists():
        backup.write_bytes(original)
    tmp = path.with_name(path.name + ".new")
    tmp.write_bytes(patched)
    check = PE(tmp.read_bytes())
    validate_patched(check)
    tmp.replace(path)

    manifest = {
        "patch": "kotor-windowed-1280",
        "format": "1",
        "magic": PATCH_MAGIC.decode(),
        "original_sha256": digest,
        "patched_sha256": sha256(patched),
        "backup": str(backup),
        "text_virtual_size": hex(TEXT_VS_NEW),
        "validator_cave": hex(IMAGE_BASE + VALIDATOR_CAVE_RVA),
        "tooltip_cave": hex(IMAGE_BASE + TOOLTIP_CAVE_RVA),
        "hud_cave": hex(IMAGE_BASE + HUD_CAVE_RVA),
        "changes": [
            {"rva": hex(VALIDATOR_RVA), "purpose": "accept 1280x720 in FUN_005f0c60"},
            {"rva": hex(TOOLTIP_CMP_RVA), "purpose": "select tooltip12X9 for 1280x720"},
            {"rva": hex(HUD_BRANCH_RVA), "purpose": "select the 1280 HUD resources for 1280x720"},
        ],
    }
    path.with_name(path.name + ".windowed1280.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def validate_patched(pe: PE) -> None:
    if pe.section(".text")["vs"] != TEXT_VS_NEW:
        raise SystemExit("patched .text VirtualSize mismatch")
    if pe.get_bytes(VALIDATOR_RVA, 1) != b"\xe9":
        raise SystemExit("validator jump missing")
    if pe.get_bytes(TOOLTIP_CMP_RVA, 1) != b"\xe9":
        raise SystemExit("tooltip jump missing")
    cave1 = pe.get_bytes(VALIDATOR_CAVE_RVA, len(build_validator_cave()))
    if cave1 != build_validator_cave():
        raise SystemExit("validator cave mismatch")
    cave2 = pe.get_bytes(TOOLTIP_CAVE_RVA, len(build_tooltip_cave()))
    if cave2 != build_tooltip_cave():
        raise SystemExit("tooltip cave mismatch")
    cave3 = pe.get_bytes(HUD_CAVE_RVA, len(build_hud_cave()))
    if cave3 != build_hud_cave():
        raise SystemExit("HUD cave mismatch")


def check(path: Path) -> dict:
    data = path.read_bytes()
    digest = sha256(data)
    status = "unknown"
    if digest == ORIGINAL_SHA256:
        status = "original"
    else:
        try:
            pe = PE(data)
            if pe.get_bytes(VALIDATOR_RVA, 1) == b"\xe9" and \
               pe.get_bytes(TOOLTIP_CMP_RVA, 1) == b"\xe9" and \
               pe.get_bytes(HUD_BRANCH_RVA, 1) == b"\xe9" and \
               pe.section(".text")["vs"] == TEXT_VS_NEW:
                validate_patched(pe)
                status = "patched"
        except Exception:
            pass
    return {"path": str(path), "sha256": digest, "status": status,
            "backup_exists": path.with_name(path.name + ".orig-1.0.3.0").exists()}


def revert(path: Path) -> dict:
    backup = path.with_name(path.name + ".orig-1.0.3.0")
    if not backup.exists():
        raise SystemExit("no backup swkotor.exe.orig-1.0.3.0 to restore")
    data = backup.read_bytes()
    if sha256(data) != ORIGINAL_SHA256:
        raise SystemExit("backup hash does not match 1.0.3.0")
    tmp = path.with_name(path.name + ".restore")
    tmp.write_bytes(data)
    tmp.replace(path)
    manifest = path.with_name(path.name + ".windowed1280.json")
    if manifest.exists():
        manifest.unlink()
    return {"path": str(path), "sha256": sha256(data), "status": "original"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", required=True, type=Path)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--apply", action="store_true")
    g.add_argument("--revert", action="store_true")
    g.add_argument("--check", action="store_true")
    args = ap.parse_args()
    exe = args.exe.resolve()
    if not exe.exists():
        raise SystemExit(f"missing {exe}")
    if args.apply:
        result = apply(exe)
    elif args.revert:
        result = revert(exe)
    else:
        result = check(exe)
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
