# Recon 04 — Geometry and orientation findings (m01aa / Endar Spire)

Read-only probe of `end_m01aa`/`m01aa`. Extracted resources and local analysis scripts stay under
`artifacts/` (gitignored); no game run and no Ghidra edits. The public parser is
[`tools/formats/gff.py`](../tools/formats/gff.py); the derived fixture descriptions are summarized
below. Verified = checked directly against shipped bytes; inferred = follows from verified data plus
a stated assumption; open = discriminating evidence still missing.

> **Parser correction:** the first public GFF parser treated every struct's `data_or_offset` as a
> byte offset. The upstream Kaitai `GFF.ksy` rule (and the files) is: `field_count == 1` means the
> value is a **direct field-table row**; `field_count > 1` means a byte offset into `field_indices`.
> The public parser now implements this and validates IFO, GIT, and ARE files.

## 0. GFF parser correction (verified, critical)

MIT Kaitai `GFF.ksy` `struct_entry` states: **field_count==1 ⇒ direct field-table row; >1 ⇒ byte
offset into field-index**. Every IFO has a single-field struct (`Mod_Area_list` element), so the
initial parser rejected all 117 IFOs; after the public fix, `tools/formats/gff.py` parses the sample
IFO (`structs=2, fields=44, struct1 single row 43`) and validates all three sample types. ARE/GIT
have no single-field structs, so the earlier ARE/GIT evidence stands; UTC/UTD templates are affected.

## 1. IFO entry (verified)

IFO values: `m01aa`, (15.4248, 20.1215, −1.2750), Dir (0.0,1.0), |Dir|=1.000000. The point is inside a room `01a` WOK floor
triangle at z=−1.275 (not 02a). All 117 IFOs: |Dir|=1 (max err 2.7e−8), entry area matches an ARE, angles
on the 5.625° grid (117/117; one half-step off 11.25°). So X/Y/Z is a **ground/feet point**, Dir a **unit 2D
facing vector**, no Dir_Z. No IFO
camera/style fields; ARE CameraStyle=0; GIT has 40 cameras. `Mod_Area_list` = [`m01aa`]. Trask is 7.475 m
from entry, facing it with dot 0.947.

## 2. GIT orientation (verified/inferred)

- **Creature**: `XOrientation,YOrientation` = unit forward vector. 26/26 |v|=1 (err 2.3e−8), angles on the
  11.25° grid (max dev 0.004°), all quadrants. Not degrees/quaternion.
- **Door/placeable**: `Bearing` = scalar yaw in **radians**, same grid (60/60 placeables, max dev 0.008°;
  range ±180°); `−0.0` is 0. For all 15 doors, `Bearing mod 180` equals the LYT hook yaw plane and the
  nearest WOK perimeter-edge angle (mod 180) within 0.2°. Facing deltas are 0°/180°: **use GIT Bearing for
  the instance, not the hook's facing**.
- **Sign/zero (inferred)**: RH, +Y-front model gives `forward=(−sin B, cos B)` with +X width. The
  `plc_chair2` PWK use hook is local −X; three chairs against 01a's north wall face south into the room
  only under RH, while under LH they face through the wall.
- **Door Z**: −0.04076 m below the WOK floor; do not snap. LYT/GIT XY: ≤1 mm for 14/15, one 51 mm; use GIT.

## 3. LYT/VIS, WOK, PTH, units (verified)

- LYT: 15 room origins and 15 doorhooks. A room origin is the **MDL model origin**: for all 15 Door_* nodes,
  `origin+node.pos` equals the LYT hook and GIT door (max err 0.000731 m). VIS is visibility text only.
- WOK `01a`: type 1, vertices @136, faces @1504, normals @4624. Vertices are **already world**: adjacent
  rooms share exact vertices where they meet. Do **not** add LYT origin or `WOK.position` (a
  centroid/pivot hint). Raw floors contain the entry, Trask, and all PTH nodes.
- **Handedness**: 1365/1365 WOK faces have `cross(v1−v0,v2−v0)·normal > 0` (CCW front, +Z up): **RH Z-up**.
- PTH: 77 `Path_Points` with X,Y only, all inside room floors (0 misses); Z must come from WOK.
- **Units**: `pmbam`/`n_commm` are 1.581/1.585 m tall, feet z≈0; room 01a floor −1.275 to ceiling 1.700
  (2.975 m); door spacing ~2.0 m. **1 unit = 1 m**.

## 4. MDL/MDX (verified/bounded)

`m01aa_01a.mdl` (383,564 B): 12-byte file header; geometry header @12, K1-PC id 0x413670, root @data 2722,
78 nodes. Door_16 node @file 3054, pos (5.41669,−1.52151,−1.31576), quat (0.70711,0,0,−0.70711). Quaternion
order **W,X,Y,Z** (exact raw match with LYT; all 40 camera quats yaw-only). Supermodel `NULL`;
animation_count 1 but no anim nodes; node transforms are the static bind pose. **Model-header bbox is a
dummy** (−5,−5,−1)..(5,5,10) for every model checked — never use it. Trimesh bboxes are node-local (floor
and ceiling meshes land on WOK z=−1.275/1.700 after node translation). Local→world:
`world = T(room_origin)·T_node(pos)·R_node(quat)` accumulated parent→child, no scale. Door nodes verify the
chain exactly. **MDX layout is decoded in `docs/recon/15_MDX_LAYOUT.md`** (no header; blocks
addressed by MDL `+324`; strides 24/32/40; 634 sampled blocks verified).

## 5. Camera (inferred)

Camera = `Position` (vec3), `Pitch`, `Orientation` (WXYZ yaw-only), `Height`, `FOV`. Position.z matches the
WOK floor within 1 mm for 32/40 cameras (37/40 are over a floor); `Height` 0.05-3.1 m, so
`eye=Position.z+Height` is plausible. CameraID 2 has yaw +78.75°, local +Y forward (−0.981,0.195), and
points at the IFO entry (dot 0.985); eye 0.225 vs player head 0.305. Supports RH yaw, +Y front,
Position=ground.

## 6. Open evidence / traps

1. Bearing sign/zero: resolved; RH `R_z(+B)` is oracle-backed (`docs/recon/12`).
2. MDX vertex/face blocks: decoded (`docs/recon/15`); animation controller payloads remain open.
3. `WOK.position` is the room walkmesh node (type 545) local translation, not a placement term;
   ARE `NorthAxis` is absent (`docs/recon/12`). ARE Map minimap affine is decoded.
4. Door appearance → model/facing: resolved via UTD `GenericType` → `genericdoors.2da` (`docs/recon/12`).
5. Camera `Height`; animation/supermodel bind-pose overrides.
6. `gff.py` single-field bug; use `c2_gff.py`.
7. **Traps:** double LYT origin on WOK; dummy MDL bbox; Bearing as degrees/quat; hook facing instead of
   Bearing; door Z snap; naive Y-up swap.

## 7. Validation fixtures

| ID | Status | Checks |
|---|---|---|
| T01 | verified | GFF single-field struct rule; IFO parses with `struct1.single` direct row 43 |
| T02 | verified | IFO entry is a ground/feet point inside 01a's WOK floor (z = −1.275) |
| T03 | verified | `Mod_Entry_Dir` is a unit 2D facing vector; 117/117 IFOs on the 5.625° grid |
| T04 | verified | Creature `XOrientation/YOrientation` is a unit forward vector; 26/26 on the 11.25° grid |
| T05 | verified | Room origin + MDL door node = LYT hook = GIT door (15/15, ≤0.731 mm) |
| T06 | verified | MDL/LYT/camera quaternions are `(W,X,Y,Z)`; door-chain cross-check |
| T07 | verified | WOK vertices are world-space; do not add LYT origin or `WOK.position` |
| T08 | verified | WOK winding/normals are right-handed, +Z-up (1365/1365 faces) |
| T09 | verified | PTH is 2D world space; all 77 sample nodes fall on WOK floors; Z from WOK |
| T10 | verified | Humanoid calibration gives 1 unit = 1 m (1.581/1.585 m heights) |
| T11 | inferred | Camera yaw is RH/+Y-forward; `Position` is ground and `Height` is eye offset |
| T12 | verified | Bearing is a radian yaw; its plane matches the wall/hook; sign is inferred |

The machine-readable fixtures (resource offsets, exact samples) stay local under
`artifacts/analysis/coordinates2/fixtures.json`; they are not committed. Verify GFF files with
`python tools/formats/gff.py <file> --check --root`; the local `c2_*.py` scripts are analysis
artifacts, not part of the public toolset.

## 8. Remaining evidence

Animation controller keyframe payloads and supermodel override mechanics (header survey in
`docs/recon/12`); minimap/DLG camera transforms; camera aspect/near/far; MDX trailing LOD/skin data
and multi-group descriptors (`docs/recon/15`).
