#!/usr/bin/env python3
"""Build lane-local synthetic save and VFS fixtures.

Everything written here is fabricated; no bytes from the game install are
copied.  The fixture tree is deterministic so tests can assert exact winners.
"""
from __future__ import annotations

import argparse
import shutil
import struct
import sys
from pathlib import Path

LANE_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = Path(__file__).resolve().parents[2]
for _p in (REPO_ROOT / "tools" / "resources", REPO_ROOT / "tools" / "formats",
           REPO_ROOT / "tools" / "inventory"):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

import kotor_archive as ka  # noqa: E402
import kotor_save as ks  # noqa: E402
from kotor_gff_write import GffStruct  # noqa: E402

FIXTURES = REPO_ROOT / "artifacts" / "tests" / "fixtures"
SAVE_ROOT = FIXTURES / "save"
VFS_ROOT = FIXTURES / "vfs"
VFS_GAME = VFS_ROOT / "game"


def _loc(text: str, strref: int = 0xFFFFFFFF) -> dict:
    return {"strref": strref, "substrings": [(0, text)]}


def make_tga_1x1() -> bytes:
    header = bytes([
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    ]) + struct.pack("<HH", 1, 1) + bytes([24, 0x20])
    return header + b"\x10\x20\x30"


def save_gffs():
    ifo = GffStruct(0xFFFFFFFF)
    ifo.add("Mod_ID", "binary", b"SYNTHETIC-MODULE")
    ifo.add("Mod_Version", "uint32", 3)
    ifo.add("Mod_Name", "locstring", _loc("Synthetic Module"))
    ifo.add("Mod_Tag", "string", "MODULE")
    ifo.add("Mod_IsSaveGame", "uint8", 1)
    ifo.add("Mod_Entry_Area", "resref", "m99aa")
    ifo.add("Mod_Entry_X", "float", 1.5)
    ifo.add("Mod_Entry_Y", "float", -2.5)
    ifo.add("Mod_Entry_Z", "float", 0.25)
    ifo.add("Mod_Entry_Dir_X", "float", 0.0)
    ifo.add("Mod_Entry_Dir_Y", "float", 1.0)
    area_ref = GffStruct(0)
    area_ref.add("Area_Name", "resref", "m99aa")
    ifo.add("Mod_Area_list", "list", [area_ref])
    ifo.add("Mod_StartHour", "uint8", 13)
    ifo.add("Mod_StartDay", "uint8", 1)
    ifo.add("Mod_StartMonth", "uint8", 6)
    ifo.add("Mod_StartYear", "uint32", 0)
    ifo.add("Mod_MinPerHour", "uint8", 2)
    ifo.add("Mod_XPScale", "uint8", 10)
    ifo.add("Mod_Description", "locstring", {"strref": 0xFFFFFFFF, "substrings": []})

    are = GffStruct(0xFFFFFFFF)
    are.add("Tag", "string", "m99aa")
    are.add("Name", "locstring", _loc("Synthetic Area"))
    are.add("Flags", "uint32", 7)
    are.add("CameraStyle", "int32", 0)
    are.add("LoadScreenID", "uint16", 0)
    room = GffStruct(0)
    room.add("RoomName", "resref", "room1")
    are.add("Rooms", "list", [room])
    are.add("Expansion_List", "list", [])

    def creature(idx: int) -> GffStruct:
        c = GffStruct(4)
        c.add("TemplateResRef", "resref", f"creature{idx}")
        c.add("XPosition", "float", float(idx))
        c.add("YPosition", "float", float(idx + 1))
        c.add("ZPosition", "float", 0.0)
        c.add("XOrientation", "float", 0.0)
        c.add("YOrientation", "float", 1.0)
        return c

    def placeable(idx: int) -> GffStruct:
        p = GffStruct(9)
        p.add("TemplateResRef", "resref", f"placeable{idx}")
        p.add("XPosition", "float", 10.0 + idx)
        p.add("YPosition", "float", 20.0)
        p.add("ZPosition", "float", 0.0)
        p.add("Bearing", "float", 1.5707964)
        return p

    def door() -> GffStruct:
        d = GffStruct(8)
        d.add("TemplateResRef", "resref", "door1")
        d.add("Tag", "string", "door1")
        return d

    git = GffStruct(0xFFFFFFFF)
    git.add("UseTemplates", "uint8", 0)
    area_props = GffStruct(1)
    area_props.add("AmbientSndDay", "uint16", 0)
    git.add("AreaProperties", "struct", area_props)
    git.add("Creature List", "list", [creature(1), creature(2)])
    git.add("Door List", "list", [door()])
    git.add("Placeable List", "list", [placeable(1), placeable(2)])
    git.add("TriggerList", "list", [])
    git.add("Encounter List", "list", [])
    git.add("StoreList", "list", [])
    git.add("SoundList", "list", [])
    git.add("WaypointList", "list", [])
    git.add("CameraList", "list", [])

    nfo = GffStruct(0xFFFFFFFF)
    nfo.add("AREANAME", "string", "Synthetic Area")
    nfo.add("LASTMODULE", "string", "m99aa")
    nfo.add("SAVEGAMENAME", "string", "Synthetic Save")
    nfo.add("PCNAME", "string", "Test Player")
    nfo.add("TIMEPLAYED", "uint32", 123)
    nfo.add("TIMESTAMP", "uint64", 133000000000000000)
    nfo.add("CHEATUSED", "uint8", 0)
    nfo.add("GAMEPLAYHINT", "uint8", 1)
    nfo.add("STORYHINT", "uint8", 2)
    nfo.add("PORTRAIT0", "resref", "po_pc")
    nfo.add("PORTRAIT1", "resref", "po_party1")
    nfo.add("PORTRAIT2", "resref", "po_party2")
    nfo.add("SCREENSHOT", "resref", "loadscreen")
    for i in range(1, 7):
        nfo.add(f"LIVE{i}", "string", f"live-{i}")

    pt = GffStruct(0xFFFFFFFF)
    pt.add("PT_GOLD", "uint32", 1234)
    pt.add("PT_NUM_MEMBERS", "uint8", 2)
    members = []
    for leader, mid in ((1, 0), (0, 1)):
        row = GffStruct(0)
        row.add("PT_IS_LEADER", "uint8", leader)
        row.add("PT_MEMBER_ID", "int32", mid)
        members.append(row)
    pt.add("PT_MEMBERS", "list", members)
    avail = []
    influence = []
    for available, select, inf in ((1, 1, 50), (1, 0, 25), (0, 0, 0)):
        row = GffStruct(0)
        row.add("PT_NPC_AVAIL", "uint8", available)
        row.add("PT_NPC_SELECT", "uint8", select)
        avail.append(row)
        inf_row = GffStruct(0)
        inf_row.add("PT_NPC_INFLUENCE", "int32", inf)
        influence.append(inf_row)
    pt.add("PT_AVAIL_NPCS", "list", avail)
    pt.add("PT_INFLUENCE", "list", influence)
    journal = []
    for plot, state, date, time in (("quest_a", 1, 100, 10), ("quest_b", 2, 200, 20)):
        row = GffStruct(0)
        row.add("JNL_PlotID", "string", plot)
        row.add("JNL_State", "int32", state)
        row.add("JNL_Date", "uint32", date)
        row.add("JNL_Time", "uint32", time)
        journal.append(row)
    pt.add("JNL_Entries", "list", journal)
    pt.add("PT_ITEM_CHEMICAL", "uint32", 4)
    pt.add("PT_ITEM_COMPONEN", "uint32", 5)

    def cat(names):
        rows = []
        for name in names:
            row = GffStruct(0)
            row.add("Name", "string", name)
            rows.append(row)
        return rows

    gv = GffStruct(0xFFFFFFFF)
    gv.add("CatNumber", "list", cat(["num_a", "num_b", "num_c"]))
    gv.add("CatBoolean", "list", cat(["bool_a", "bool_b", "bool_c"]))
    gv.add("CatString", "list", cat(["str_a", "str_b"]))
    gv.add("CatLocation", "list", cat(["loc_a"]))
    gv.add("ValNumber", "binary", bytes([42, 7, 255]))
    gv.add("ValBoolean", "binary", bytes([0b10100000]))
    strings = []
    for value in ("alpha", "beta"):
        row = GffStruct(0)
        row.add("String", "string", value)
        strings.append(row)
    gv.add("ValString", "list", strings)
    gv.add("ValLocation", "binary", struct.pack("<6f", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0))

    pifo = GffStruct(0xFFFFFFFF)
    pifo.add("Mod_Tag", "string", "PIFO")
    pifo.add("Mod_Area_list", "list", [GffStruct(0).add("Area_Name", "resref", "m99aa")])

    repute = GffStruct(0xFFFFFFFF)
    fac = []
    for name, parent in (("faction_a", -1), ("faction_b", 0)):
        row = GffStruct(0)
        row.add("FactionName", "string", name)
        row.add("FactionParentID", "int32", parent)
        fac.append(row)
    repute.add("FactionList", "list", fac)
    rep = []
    for faction_id, value in ((0, 50), (1, 25), (2, 10)):
        row = GffStruct(0)
        row.add("FactionID", "int32", faction_id)
        row.add("FactionRep", "int32", value)
        rep.append(row)
    repute.add("RepList", "list", rep)

    inv = GffStruct(0xFFFFFFFF)
    items = []
    for res, x, y in (("item_a", 1, 2), ("item_b", 3, 4)):
        row = GffStruct(0)
        row.add("InventoryRes", "resref", res)
        row.add("Repos_PosX", "uint16", x)
        row.add("Repos_PosY", "uint16", y)
        items.append(row)
    inv.add("ItemList", "list", items)

    av = GffStruct(0xFFFFFFFF)
    av.add("Tag", "string", "availnpc0")
    av.add("FirstName", "locstring", _loc("Synthetic NPC"))
    av.add("Available", "uint8", 1)

    pc = GffStruct(0xFFFFFFFF)
    pc.add("Tag", "string", "pc")
    pc.add("FirstName", "string", "Test")
    pc.add("LastName", "string", "Player")
    pc.add("InventoryList", "list", [])
    pc.add("Equip_ItemList", "list", [])

    return {
        "ifo": ifo, "are": are, "git": git, "nfo": nfo, "pt": pt, "gv": gv,
        "pifo": pifo, "repute": repute, "inv": inv, "avail": av, "pc": pc,
        "screen": make_tga_1x1(),
    }


def build_save_fixture() -> list[str]:
    if SAVE_ROOT.exists():
        shutil.rmtree(SAVE_ROOT)
    SAVE_ROOT.mkdir(parents=True)
    g = save_gffs()
    out = []

    def build(folder: str, autosave: bool) -> None:
        w = ks.SaveWriter(SAVE_ROOT / folder)
        w.add_module("m99aa", g["ifo"], g["git"], are=g["are"])
        w.add_outer_gff("REPUTE", 2038, b"FAC ", g["repute"])
        w.add_outer_gff("INVENTORY", 2037, b"INV ", g["inv"])
        w.add_outer_gff("AVAILNPC0", 2027, b"UTC ", g["avail"])
        w.add_outer_gff("PC", 2015, b"BIC ", g["pc"])
        w.add_sidecar_gff("savenfo.res", b"NFO ", g["nfo"])
        w.add_sidecar_gff("PARTYTABLE.res", b"PT  ", g["pt"])
        w.add_sidecar_gff("GLOBALVARS.res", b"GVT ", g["gv"])
        if autosave:
            w.add_sidecar_gff("pifo.ifo", b"IFO ", g["pifo"])
        else:
            w.add_sidecar_bytes("Screen.tga", g["screen"])
        w.write()
        out.append(str(SAVE_ROOT / folder))

    build("000001 - AUTOSAVE", autosave=True)
    build("000002 - SYNTHETIC SAVE", autosave=False)
    # Corrupt sentinel fixture is a copy plus the empty sentinel.
    src = SAVE_ROOT / "000002 - SYNTHETIC SAVE"
    corrupt = SAVE_ROOT / "000003 - CORRUPT TEST"
    shutil.copytree(src, corrupt)
    (corrupt / "CORRUPT.res").write_bytes(b"")
    out.append(str(corrupt))
    return out


def _write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def build_vfs_fixture() -> str:
    if VFS_ROOT.exists():
        shutil.rmtree(VFS_ROOT)
    VFS_GAME.mkdir(parents=True)
    for sub in ("Override", "streamwaves", "streammusic", "movies", "rims",
                "modules", "lips", "TexturePacks"):
        (VFS_GAME / sub).mkdir(parents=True, exist_ok=True)
    # Pass 1: directories.
    _write(VFS_GAME / "Override" / "tier_probe.tga", b"OVERRIDE_TIER")
    _write(VFS_GAME / "Override" / "Shared.TGA", b"OVERRIDE_SHARED")
    _write(VFS_GAME / "Override" / "dual_probe.tga", b"DUAL_TGA")
    _write(VFS_GAME / "Override" / "dual_probe.tpc", b"DUAL_TPC")
    _write(VFS_GAME / "Override" / "wav_probe.wav", b"OVERRIDE_WAV")
    _write(VFS_GAME / "streamwaves" / "wav_probe.wav", b"STREAMWAVES_WAV")
    _write(VFS_GAME / "streammusic" / "music_probe.wav", b"STREAMMUSIC_WAV")
    _write(VFS_GAME / "movies" / "movie_probe.bik", b"MOVIE_BIK")
    # Pass 2: ERFs tagged 1.
    ka.write_erf(VFS_GAME / "patch.erf", [
        ("tier_probe", 3, b"PATCH_TIER"),
        ("patch_only", 2017, b"PATCH_ONLY"),
        ("dup_probe", 2017, b"DUP_FIRST"),
        ("dup_probe", 2017, b"DUP_SECOND"),
        ("patch_vs_rim", 2010, b"PATCH_VS_RIM"),
        ("patch_vs_tex", 3007, b"PATCH_TEX"),
        ("case_probe", 2017, b"PATCH_CASE"),
    ])
    ka.write_erf(VFS_GAME / "Override" / "textures.erf", [
        ("tex_erf_probe", 3007, b"TEX_ERF"),
        ("tier_probe", 3, b"TEX_ERF_TIER"),
        ("patch_vs_tex", 3007, b"TEX_ERF_WINS"),
    ])
    # Pass 3: RIMs.
    ka.write_rim(VFS_GAME / "rims" / "global.rim", [
        ("global_probe", 2010, b"GLOBAL_RIM"),
        ("mod_vs_rim", 2017, b"GLOBAL_MOD_VS_RIM"),
        ("case_probe", 2017, b"CASE_FIRST"),
        ("CaseProbe", 2017, b"CASE_SECOND"),
        ("RimCase", 2017, b"RIM_CASE_FIRST"),
        ("rimcase", 2017, b"RIM_CASE_SECOND"),
        ("dup_rim", 2010, b"RIM_FIRST"),
        ("dup_rim", 2010, b"RIM_SECOND"),
        ("rim_vs_bif", 2017, b"GLOBAL_RIM"),
        ("tier_probe", 3, b"GLOBAL_TIER"),
    ])
    ka.write_rim(VFS_GAME / "modules" / "active.rim", [
        ("area_probe", 2012, b"AREA_RIM"),
        ("area_vs_static", 2010, b"AREA_RIM"),
        ("save_vs_rim", 2010, b"AREA_RIM"),
    ])
    ka.write_rim(VFS_GAME / "modules" / "active_s.rim", [
        ("static_probe", 2010, b"STATIC_RIM"),
        ("area_vs_static", 2010, b"STATIC_RIM"),
        ("mod_vs_rim", 2017, b"STATIC_RIM"),
        ("save_vs_rim", 2010, b"STATIC_RIM"),
        ("patch_vs_rim", 2010, b"STATIC_RIM"),
    ])
    # Pass 4: ERFs tagged 2.
    ka.write_erf(VFS_GAME / "modules" / "active.mod", [
        ("mod_only", 2011, b"MOD_ONLY"),
        ("mod_vs_texture", 3007, b"MOD_TEX"),
        ("mod_vs_save", 2017, b"MOD_VS_SAVE"),
        ("tier_probe", 3, b"MOD_TIER"),
        ("mod_vs_rim", 2017, b"MOD_RIM"),
        ("mod_vs_bif", 2017, b"MOD_ERF"),
    ])
    ka.write_erf(VFS_GAME / "lips" / "localization.mod", [
        ("lips_probe", 2029, b"LIPS_LOCALIZATION"),
    ])
    ka.write_erf(VFS_GAME / "lips" / "active_loc.mod", [
        ("lips_loc_probe", 2029, b"LIPS_ACTIVE"),
    ])
    texture = [
        ("swpc_tex_tpa.erf", "TPA"),
        ("swpc_tex_tpb.erf", "TPB"),
        ("swpc_tex_tpc.erf", "TPC"),
    ]
    for filename, tag in texture:
        ka.write_erf(VFS_GAME / "TexturePacks" / filename, [
            ("tex_probe", 3007, f"{tag}_TEX".encode()),
            ("mod_vs_texture", 3007, f"{tag}_MODTEX".encode()),
            ("quality_probe", 3007, (f"{tag}_QUALITY_" + "X" * (ord(tag[2]) - ord("A"))).encode()),
        ])
    ka.write_erf(VFS_GAME / "TexturePacks" / "swpc_tex_gui.erf", [
        ("gui_tex_probe", 3007, b"GUI_TEX"),
    ])
    # Pass 5: chitin.key + BIF.
    ka.write_key_bif(VFS_GAME, [
        ("base_probe", 2017, b"BASE_TIER"),
        ("tier_probe", 3, b"BASE_TIER"),
        ("rim_vs_bif", 2017, b"BASE_RIM"),
        ("tex_probe", 3007, b"BASE_TEX"),
        ("case_probe", 2017, b"BASE_CASE"),
        ("mod_vs_bif", 2017, b"BASE_MOD"),
    ])
    # Save tier: a nested 'active' module ERF inside MOD-tagged SAVEGAME.sav.
    save_dir = VFS_GAME / "saves" / "000002 - TEST"
    save_dir.mkdir(parents=True)
    nested = ka.erf_bytes([
        ("save_probe", 2010, b"SAVE_ERF"),
        ("mod_vs_save", 2017, b"SAVE_ERF"),
        ("save_only", 2010, b"SAVE_ERF"),
        ("save_vs_rim", 2010, b"SAVE_ERF"),
    ], magic=b"MOD ")
    outer = ka.erf_bytes([("active", 2057, nested)], magic=b"MOD ")
    (save_dir / "SAVEGAME.sav").write_bytes(outer)
    return str(VFS_ROOT)


def build_all() -> dict:
    saves = build_save_fixture()
    vfs = build_vfs_fixture()
    return {"saves": saves, "vfs": vfs}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", help="optional status JSON path")
    args = ap.parse_args(argv)
    result = build_all()
    import json
    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
