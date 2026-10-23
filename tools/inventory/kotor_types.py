"""KotOR 1 resource type IDs.

Confidence:
- "signature": payload magic observed in the user's install by resource_probe.py.
- "spec": numeric ID/role from independent implementations (xoreos types.h and
  KotOR.js ResourceTypes.ts agree), not necessarily present in this install.
- "inferred": role inferred from payload/context; still needs a parser or runtime check.

Evidence for the table: docs/recon/01_ARTIFACTS.md and the xoreos/KotOR.js type tables.
"""

TYPE_INFO: dict[int, tuple[str, str, str]] = {
    0: ("RES", "spec", "invalid/reserved"),
    3: ("TGA", "signature", "Targa image; lightmaps paired with TXI"),
    4: ("WAV", "signature", "RIFF/WAVE audio"),
    7: ("INI", "spec", "configuration text"),
    8: ("BMU", "spec", "MP3 with BMU prefix"),
    10: ("TXT", "spec", "text"),
    2002: ("MDL", "signature", "binary model"),
    2009: ("NSS", "signature", "NWScript source text"),
    2010: ("NCS", "signature", "compiled NWScript bytecode (big-endian)"),
    2011: ("MOD", "spec", "module ERF"),
    2012: ("ARE", "signature", "area GFF"),
    2014: ("IFO", "signature", "module info GFF"),
    2015: ("BIC", "signature", "player character GFF"),
    2016: ("WOK", "inferred", "BWM V1.0 area walkmesh"),
    2017: ("2DA", "signature", "tabular 2DA text (V2.b)"),
    2018: ("TLK", "spec", "talk table"),
    2022: ("TXI", "signature", "texture info text (islightmap)"),
    2023: ("GIT", "signature", "instance list GFF"),
    2024: ("BTI", "inferred", "binary item template; semantics unresolved"),
    2025: ("UTI", "signature", "item template GFF"),
    2026: ("BTC", "inferred", "binary creature template; semantics unresolved"),
    2027: ("UTC", "signature", "creature template GFF"),
    2029: ("DLG", "signature", "dialogue GFF"),
    2030: ("ITP", "signature", "item/creature palette"),
    2032: ("UTT", "signature", "trigger template GFF"),
    2033: ("DDS", "spec", "DirectDraw surface texture"),
    2035: ("UTS", "signature", "sound emitter template GFF"),
    2036: ("LTR", "inferred", "letter/name-generation table (humanf)"),
    2037: ("GFF", "spec", "generic GFF"),
    2038: ("FAC", "signature", "faction table GFF"),
    2040: ("UTE", "signature", "encounter template GFF"),
    2042: ("UTD", "signature", "door template GFF"),
    2044: ("UTP", "signature", "placeable template GFF"),
    2047: ("GUI", "signature", "GUI definition GFF"),
    2051: ("UTM", "signature", "merchant template GFF"),
    2052: ("DWK", "inferred", "BWM V1.0 door walkmesh"),
    2053: ("PWK", "inferred", "BWM V1.0 placeable walkmesh"),
    2056: ("JRL", "signature", "journal/quest GFF"),
    2057: ("SAV", "spec", "save game archive"),
    2058: ("UTW", "signature", "waypoint template GFF"),
    2060: ("SSF", "signature", "sound set (28 StrRefs)"),
    2063: ("BIK", "spec", "Bink video"),
    3000: ("LYT", "signature", "layout text (#MAXLAYOUT ASCII)"),
    3001: ("VIS", "signature", "room visibility text"),
    3002: ("RIM", "spec", "module archive"),
    3003: ("PTH", "signature", "pathfinding GFF"),
    3004: ("LIP", "spec", "lip-sync data"),
    3007: ("TPC", "spec", "BioWare texture"),
    3008: ("MDX", "signature", "model vertex/animation data"),
    9997: ("ERF", "spec", "ERF archive"),
    9998: ("BIF", "spec", "BIF archive"),
    9999: ("KEY", "spec", "resource index"),
}


def label(rtype: int) -> str:
    info = TYPE_INFO.get(rtype)
    return info[0] if info else "UNKNOWN"


def describe(rtype: int) -> str:
    info = TYPE_INFO.get(rtype)
    if not info:
        return f"{rtype}:UNKNOWN"
    return f"{rtype}:{info[0]} ({info[1]})"
