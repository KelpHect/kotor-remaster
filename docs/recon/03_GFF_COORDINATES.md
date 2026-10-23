# Recon 03 — GFF wire and coordinate findings (m12ab)

Read-only probe: [`tools/formats/gff.py`](../tools/formats/gff.py). Local inputs were
`m12ab.git` (2,142 B) and `m12ab.are` (11,181 B), extracted from the user's install; they are not
committed. No game run, no Ghidra, no project edits. This document records wire semantics and
coordinate observations, not a finished conversion contract.

## 1. Wire semantics (verified)

Spec anchors are in the MIT Kaitai `bioware_gff_common.ksy`
(OpenKOTOR/bioware-kaitai-formats): `GFFFieldData.field_type` at **+0**, the per-type
inline-vs-field-data rules with `data_or_data_offset` at +8, struct values as
`GFFStructData` indices (12-byte entries), and lists as byte offsets into the list-index arena.
The upstream GIT/ARE `.ksy` capsules delegate wire layout to `formats/GFF/GFF.ksy`; the full
`GFF.ksy` was not downloaded for this probe (see section 5).

**Header** (56 B): `magic[4] + version[4]`, then 12 x u32 in order
struct off/count, field off/count, label off/count, field-data off/**byte len**,
field-index off/**byte len**, list-index off/**byte len**. GIT: 56 / 212 /
1124 / 1604+150 / 1754+304 / 2058+84 = 2142 = EOF. ARE: 56 / 716 / 5756 /
8188+1133 / 9321+1680 / 11001+180 = 11181 = EOF.

**Struct** (12 B): `u32 type_id, u32 field_index_byte_offset, u32 field_count`.
Root is `0xffffffff` at offset 0 (GIT @0x38, ARE @0x38). Byte offset / 4 gives
the field-index slot; slots are packed in struct order. GIT struct 1 @0x44 =
(100, 48, 9) -> slot 12 (AreaProperties, 9 int32s).

**Field** (12 B): `u32 type, u32 label_index, u32 data_or_data_offset`.
Column order was discriminated, not assumed: reading label-first yields
47/76 (GIT) and 400/420 (ARE) type tags > 18 (invalid); type-first yields zero.
Field @0x20c = `08 000000 | 19 000000 | 0013fd3f` -> float, label 25 "X",
1.977142. Labels are 16-byte NUL-padded ASCII (GIT @0x464, 30 entries;
ARE @0x167c, 152 entries) and are **file-local** (only labels actually used).

**Field-index table**: `u32[field_count]`, slot -> field-table row. This
indirection is load-bearing: GIT root's 12 slots map to rows
0, 1, 11-15, 21-24, 75. Without it the root would lose Door/Sound/Store/Trigger/
Waypoint/Camera lists. Validation passes in both files: every field row is
referenced exactly once, slots end at field_count, no struct has duplicate
labels, sections are contiguous, list ranges pack without overlap and end at
the arena length.

**List**: value is a byte offset; record = `u32 count` then `count x u32 struct
index`. GIT SoundList @0x81a = (1,[2]); Placeable List @0x82e = (10,[3..12]);
CameraList @0x85a = 0. ARE Rooms @0x2afd = (1,[2]).

**Value locations** (only these occur here): inline types 0,2,4,5,8; field-data
types 10 (u32 len + bytes), 11 (u8 len + bytes, GIT fdata[10] = 0x0d +
"plc_invisible"), 12 (u32 total, strref, count; ARE Name strref 42364, 0
substrings), 17 (3 floats). Types 14/15 are indices/offsets.

## 2. m12ab.git — root (12 fields) and instances

| root field | type | value |
|---|---|---|
| UseTemplates | uint8 | 1 |
| AreaProperties | struct 1 (type 100) | 9 int32s: AmbientSndDay 29, AmbientSndDayVol 127, rest 0 |
| Creature List | list | count 0 |
| Door List | list | count 0 |
| Encounter List | list | count 0 |
| List | list | count 0 |
| SoundList | list | count 1, structs [2] |
| StoreList | list | count 0 |
| TriggerList | list | count 0 |
| WaypointList | list | count 0 |
| Placeable List | list | count 10, structs [3..12] |
| CameraList | list | count 0 |

Only two populated lists; no creatures, doors, waypoints, triggers, cameras.

- SoundList[0] (struct 2, type 6): TemplateResRef `custom002`,
  GeneratedType 0, XPosition 4.888580, YPosition 3.029977, ZPosition 0.991358
  (fields @0x194/0x1a0/0x1ac/0x1b8/0x1c4).
- Placeable List (struct 3-12, type 9): TemplateResRef `plc_invisible`,
  X/Y/Z/Bearing (@0x200, 0x20c, 0x218, 0x224, 0x230). Samples:
  (1.977142, 6.843895, -0.003498), (7.077698, 3.998031, -0.012508),
  (-1.475464, 5.651069, 0.002603), (1.432007, -0.620667, -0.002532).
  Every Bearing entry is 0x80000000 = IEEE **-0.0**.
- Orientation: the 30-entry label table contains "Bearing" (index 28) but no
  "XOrientation"/"YOrientation", so no creature-style orientation is testable
  in this file.

## 3. m12ab.are — coordinate-related root fields

Root struct 0 has 61 fields (ARE root @0x38 = (0xffffffff, 0, 61)).

- `Rooms` (list, 1): struct 2 -> RoomName "m12ab_01a", EnvAudio 0,
  AmbientScale 1.0. No per-room coordinates.
- `Map` (struct 1 @0x44 = (0, 244, 11)): MapResX 0, NorthAxis 0,
  WorldPt1X/Y 0.0, WorldPt2X/Y 0.0, MapPt1X/Y 0.0, MapPt2X/Y 0.0, MapZoom 1.
  Degenerate: no minimap transform. "MapResY" is absent from the label table.
- `CameraStyle` int32 0; no camera position. ARE stores no camera transform;
  GIT CameraList (the area cameras) is empty here.
- No entry-point fields exist in ARE (entry coords live in IFO, not extracted).
- `MiniGame` (struct 3 @0x5c): Type 2; camera Near_Clip 0.1, Far_Clip 2000,
  CameraViewAngle 65; track-relative Player Start/Target/Tunnel offsets
  (TunnelZ +/-9999) and Models/Gun_Banks/Enemies/Obstacles lists. No world-space
  positions.

## 4. Units, landmarks, handedness

- **Z is vertical**: 10 placeables lie within |Z| <= 0.013 while X spans 8.553
  and Y spans 7.755. An X- or Y-up reading would place the floor plane on a wall.
- **+Z upward** is supported (not proven): the sound emitter sits at Z 0.991,
  about 1 unit above the placeable plane.
- **Scale**: one room, ~8.5 x 7.8 units. Consistent with metre-scale units and
  inconsistent with cm/mm, but there is no character or door instance here
  (both lists count 0), so character/door calibration is not possible.
- **Handedness / bearing sign: unresolved.** All bearings are -0.0, no
  creatures/doors, and ARE Map calibration is zero with NorthAxis 0. Do not
  assume right/left-handedness, degrees/radians, or CW/CCW from this sample.
  No conversion is proposed.

## 5. Spec gaps (exactly what is missing)

The upstream `GIT.ksy`/`ARE.ksy` are capsules importing `../../gff` (`GFF.ksy`) and
`../../../Common/bioware_type_ids`; those imports were not downloaded. The common spec used here
defines only enum values plus per-type inline/field-data location. It does **not** define: header
order/units; struct, field, label, field-index and list record layouts; the slot->field-row
indirection; string/resref/locstring/binary prefix widths; struct type IDs or template field lists.
Those were derived from the two files and validated by the invariants above.
Value types 1,3,6,7,9,13,16,18 do not occur in these files and remain untested
(13's length-prefix width in particular is an assumption in the parser).

## 6. Open questions

1. Bearing units and sign; whether -0.0 is deliberate "unset".
2. XOrientation/YOrientation semantics (2 floats; absent from m12ab).
3. Handedness: needs nonzero ARE Map WorldPt/MapPt + NorthAxis, or a door
   bearing compared with WOK/MDL geometry.
4. Units: needs a character/door instance or WOK/MDL extents.
5. Are struct type IDs (100 AreaProperties, 6 sound, 9 placeable) template IDs
   or engine-assigned? CommonGFFStructs.pdf is not downloaded.
6. Does any shipped data use non-inline floats or vector4 (type 16)?

## 7. Proposed coordinate-contract update

```
gff.wire:
  header  = 12 u32 after 8-byte magic/version;
            struct/field/label counts are entries; data/index lengths are BYTES
  struct  = {u32 id; u32 field_index_byte_off; u32 field_count}
  field   = {u32 type; u32 label; u32 data}; label at +4, data at +8
  resolve = field_table[ field_index_table[ struct.slot + i ] ]
  list    = {u32 count; u32 struct_index[count]} at list_arena[field.data]
coords:
  up          = +Z (Z is vertical; +Z upward supported, see 4)
  units       = UNKNOWN (metre-scale plausible; no calibration object)
  handedness  = UNKNOWN; must not be assumed
  origin      = per-area world origin; no offset carried in ARE/GIT
  bearing     = float; zero observed as -0.0; units/sign UNKNOWN
  creature    = XOrientation/YOrientation (unverified; absent here)
  entry point = IFO only, not ARE/GIT
  minimap     = ARE Map.WorldPt*/MapPt*/NorthAxis (zero here => no transform)
  room        = ARE Rooms only names geometry; positions come from GIT/LYT/WOK
```

## Reproduce

Extract the resources from your own install (the extracted files are not committed), then:

```
python tools/formats/gff.py <PATH_TO_GIT> --check --root --entries
python tools/formats/gff.py <PATH_TO_ARE> --check --root
python tools/formats/gff.py <PATH_TO_ARE> --struct 1   # Map
python tools/formats/gff.py <PATH_TO_ARE> --struct 3   # MiniGame
```
