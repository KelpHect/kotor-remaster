# Recon 19 — Map compiler + hard-fail validator over all 117 areas (lane s1-03)

Status: **complete**. Build under test: `swkotor.exe` SHA-256
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6` (verified read-only at the
start of every run; the run stops on drift). All reads of
`E:/SteamLibrary/steamapps/common/swkotor` were read-only; no game assets, extracted
payloads, or decompiled program text are in tracked files — per-area reports and the
summary are under `artifacts/lanes/s1-03-mapcompile/` (gitignored).

Deliverables: `tools/mapcompile/` (`constants.py`, `parsers.py`, `loader.py`,
`validator.py`, `compile.py`), `tools/tests/test_mapcompile.py`, this doc, and
`artifacts/lanes/s1-03-mapcompile/{reports/*.json, summary.json, run.log}`.

## 1. Resource homes (verified by payload magic against the install)

| resource | type id | container | evidence |
|---|---|---|---|
| ARE / GIT / module IFO | 2012 / 2023 / 2014 (resref `module`) | `modules/<mod>.rim` | magic `ARE V3.2` / `GIT V3.2` / `IFO V3.2` |
| PTH | 3003 (resref = area) | `modules/<mod>_s.rim` | magic `PTH V3.2` |
| LYT | 3000 (resref = area) | `data/layouts.bif` | text `#MAXLAYOUT ASCII` |
| VIS | 3001 (resref = area) | `data/lightmaps*.bif` | text room graph |
| WOK (BWM) / MDL / MDX | 2016 / 2002 / 3008 (resref = room) | `data/models.bif` | magic `BWM V1.0` |
| surfacemat.2da (walkable ids) | 2017 | `data/2da.bif` | `walk` column |
| lightmap TXIs | 2022 | `data/lightmaps*.bif` | payload starts `islightmap` (5,474 textures) |

117 module pairs in `modules/` (234 RIMs) → 117 compilations; 106 distinct area resrefs
(11 areas are shared by STUNT/ebo modules, e.g. `m12aa` in `ebo_m12aa`/`ebo_m40aa`/`ebo_m40ad`/`ebo_m41aa` —
verified by scanning every main RIM's ARE resref). This matches MASTER_PLAN §3 "234 RIMs = 117 area pairs".

## 2. Frozen coordinate / transform contract

Frozen rules, each with its evidence. Status: **verified** = checked against shipped bytes
(build SHA-256 above); **inferred** = follows from verified data plus a stated assumption;
**open** = discriminating evidence still missing.

1. **Source space is right-handed, Z-up, 1 unit = 1 m.** WOK faces pass
   `cross(v1-v0, v2-v0)·normal > 0` (recon/04 T08, m01aa 1365/1365; this lane: 190,865 WOK
   faces corpus-wide, **0 inverted**, min dot 0.0 from 123 zero-area faces); humanoid
   calibration 1.581/1.585 m (recon/04 T10). ADR 0002 §4.2. — verified
2. **WOK vertices are world-space**; do not add the LYT origin or `WOK.position`
   (recon/04 T07; `WOK.position` is the room MDL's type-545 walkmesh-node local translation,
   bit-exact 453/453, recon/12). — verified
3. **LYT room origin = the room MDL model origin**; door hooks equal
   `origin + MDL door-node world pos` (recon/04 T05, 15/15 ≤ 0.731 mm). — verified
4. **MDL quaternions are W,X,Y,Z**; `world = T(room_origin)·∏ parent→child T·R`, no scale
   (recon/04 §4/T06). Model-header bbox is a dummy; trimesh bboxes are node-local. — verified
5. **MDX**: no header; per-mesh block at MDL mesh+324, vertex stride
   `24 + 8·texCount + 32·skin + 36·TBN`, texCount = mesh+304>>16, vc = mesh+304&0xFFFF,
   uv0 @+24, uv1 (lightmap) @+32 (recon/15, 76,703/76,703 blocks; lane-10 artifact
   `artifacts/lanes/10-coords-open/cont/mdx2/mdx2_mdl.py`). — verified
6. **GFF**: `field_count==1` ⇒ direct field-table row (recon/04 §0, T01); labels are
   file-local 16-byte NUL-padded (recon/03 §1). — verified
7. **IFO entry** `Mod_Entry_X/Y/Z` is a ground/feet point, `Mod_Entry_Dir_X/Y` a unit 2D
   facing vector (117/117 |Dir|=1, recon/04 T02/T03). — verified
8. **GIT Bearing** is a signed radian yaw, RH `R_z(+B)`, `forward=(−sin B, cos B)`
   (recon/12: engine `0x4ac960`, `0x5853c0`/`0x5896e0`; PWK oracle 10 RH / 0 LH);
   creature `XOrientation/YOrientation` is the unit forward vector (recon/04 T04). — verified
9. **PTH** points are 2D world space (X,Y); Z comes from the WOK (recon/04 T09);
   connections via `Path_Conections`/`First_Conection`/`Conections` (shipped key
   `Path_Conections`, MASTER_PLAN §7.1). — verified
10. **Canonical conversion** to RH Y-up happens once at the importer
    (`(x,y,z)→(x,z,−y)` with conjugated quaternions, MASTER_PLAN §7.1); never swap axes
    alone (winding flips). — verified as the contract; the cache format itself is P4.
11. **ARE Map minimap affine** uses only `WorldPt1/2` → `MapPt1/2` per axis; `NorthAxis` is
    a UI/compass selector, not part of the affine (recon/12). — verified (fields), inferred (role of NorthAxis)
12. **MapResX role** — see §6. — inferred
13. Camera `Position` is a ground point, `eye = Position.z + Height`,
    `forward = R_yaw·(0, sin Pitch, −cos Pitch)`, FOV/Pitch degrees (recon/12). — verified (formula), open (live dialogue-camera object — lane s1-06)

## 3. Validator rules (hard-fail) and triage codes

Hard rules produce a report entry and a **nonzero exit** when violated; triage codes are
reported known-outlier classes and do not fail the build. Tolerances live in
`tools/mapcompile/constants.py`.

| code | rule | hard condition |
|---|---|---|
| R1_WINDING | WOK winding vs RH Z-up convention | any type-1 face with `cross·normal < -1e-9` (inverted) |
| R2_BOUNDS | geometry vs ARE/IFO room bounds | ARE room missing from LYT; degenerate/non-finite area bounds (WOK ∪ MDL world extents); IFO entry outside bounds or `\|dz\|` to walkmesh floor > 1.0 m |
| R3_WALKMESH | walkmesh connectivity | BFS over shared edges of walkable faces (surfacemat `walk`==1) per room yields > 1 component |
| R4_PTH | path graph | any `Path_Conections` Destination outside `[0, point count)`; with points present, undirected graph not connected |
| R5_LIGHTMAP_UV | lightmap UVs | any uv1 of a lightmap-textured mesh (TXI `islightmap`) outside `[0,1]` ± 1e-6 |
| R6_ANCHOR | anchors | any GIT door/waypoint/trigger/encounter/camera outside area bounds **and** > 0.05 m off the walkmesh; door with no LYT hook within 1.0 m |
| R0_LOAD | resource load/parse failure | any exception while loading the module |

Triage codes: `T_WINDING_DEGENERATE` (zero-area faces), `T_ROOM_NO_WOK` (LYT room with no
or no-walkable WOK), `T_DOOR_HOOK_OFFSET` (door > 10 mm from its hook; known 51 mm
`end_door06` outlier, recon/11), `T_ANCHOR_OFF_WALKMESH`, `T_PTH_OFF_WALKMESH`,
`T_UV_SENTINEL` (0xFEFFEEEE, recon/15), `T_PTH_MULTI_COMPONENT` (no PTH shipped).

## 4. Landmark fixtures (synthetic, `python tools/tests/test_mapcompile.py`)

13 tests, **13 run / 13 pass / 0 fail** (2026-…, this lane): one known-good mini-area
(1 room, square walkmesh, 1 door+hook, waypoint, camera, 2-node PTH, lightmapped mesh,
IFO entry on the floor) validates `clean`; one fixture per violated rule hard-fails with
the right code: flipped face → `R1_WINDING`; IFO entry at (100,100) and ARE ghost room →
`R2_BOUNDS`; two disjoint walkable quads → `R3_WALKMESH` (2 components); PTH destination
99 and 2 isolated nodes → `R4_PTH`; uv1 = (1.5, 0.5) → `R5_LIGHTMAP_UV`; door at (100,100)
→ `R6_ANCHOR`. Triage fixtures: 51 mm-class door-hook offset → `T_DOOR_HOOK_OFFSET` (not
hard), uv1 sentinel → `T_UV_SENTINEL`. Loader fixtures build a complete synthetic game tree
(chitin.key + 4 BIFs + `tmod.rim`/`tmod_s.rim` via the lane-05 writers) and validate it
`clean`; an emptied `_s` RIM yields triage-only. Synthetic only — no game data in tests.

## 5. Full-corpus run (read-only)

```
KOTOR_GAME_DIR=E:/SteamLibrary/steamapps/common/swkotor \
  python tools/mapcompile/compile.py --out artifacts/lanes/s1-03-mapcompile
```

117/117 module pairs compiled in ~116 s; install sentinel verified; exit code 1 (hard
violations exist — see triage below). **Summary: 6 clean, 46 triaged, 65 hard_failed.**
Corpus totals: 1,472 LYT rooms, 1,279 rooms with WOK, 965 door hooks, 190,865 WOK faces,
9,551,601 lightmap uv1 values, 6,964 PTH points / 15,040 directed edges, 6,972 anchors.

| code | entries | triage of the class |
|---|---:|---|
| R1_WINDING (hard) | 0 | no inverted face in the whole corpus (min dot 0.0); 31 `T_WINDING_DEGENERATE` entries (123 zero-area faces) — shipped, recon/15 documents degenerate faces |
| R2_BOUNDS (hard) | 0 | m12ab IFO entry dz = 0.700 m (within the frozen 1.0 m tolerance); ARE⊆LYT holds everywhere |
| R3_WALKMESH (hard) | 90 rooms in 53 modules | shipped multi-level rooms whose walkable plates join through doors/PTH, not shared walkmesh edges (e.g. `m14aa_01a`: 4 components at z 0.05–9.08). R4 stays green everywhere → navigation graph intact. Verified data; classified shipped-baseline, not a compiler error |
| R4_PTH (hard) | 1 (`tar_m02ae`: 2 components, sizes 68+9) | genuine shipped split; all other 91 areas with PTH have exactly 1 component. 2 `T_PTH_OFF_WALKMESH` (tar_m08aa 0.794 m, unk_m41aa 0.676 m); 25 modules ship no PTH |
| R5_LIGHTMAP_UV (hard) | 322 entries, 1,499 values in 21 modules | 1,499 / 9,551,601 uv1 (0.016 %) outside [0,1], all on lightmap-textured meshes, quantized to a 1/16 grid, worst 6.625 (STUNT_07 `m12aa_01f` `Loft20`); consistent with lightmap wrap sampling — **open**: confirm wrap/clamp in the renderer (binary, lane s1-07). Sentinels triaged (`T_UV_SENTINEL`) |
| R6_ANCHOR (hard) | 6 | 5 static/cutscene doors with no LYT hook within 1 m (man28aa_doorstat 24.4 m, tar02_cantdoor 10.7 m, tar04_maingate 55.6 m, tat_m17aa ×2) + 1 camera `tar_m03ae` at (0,0,0), 77 m outside bounds — shipped placements, verified in GIT bytes. 33 `T_DOOR_HOOK_OFFSET` (worst 148 mm unk_m44aa; includes the known 51 mm end_door06, recon/11) |
| R0_LOAD | 0 | every module parsed |

Per-area JSON: `artifacts/lanes/s1-03-mapcompile/reports/<module>.json`
(status, entries with rule/code/severity/message/refs, stats, sources, build hash);
`summary.json` has the clean/triaged/hard-failed buckets, per-code counts, and module
lists; `run.log` is the console transcript.

## 6. Closing recon/12 "Still open" (data side)

- **MapResX role** — 117/117 AREs carry a `Map` struct. `MapResX` values are small ints
  {0,1,15,16,20,24,25,32,…}; exactly the 26 areas with `MapResX = 0` are the ones with a
  degenerate (zero-world-extent) affine — verified by reading all 117 Map structs. The
  affine itself uses only `WorldPt1/2`→`MapPt1/2` (recon/12). **Inferred:** `MapResX` is
  the minimap texture width/resolution hint; it is not part of the world→map affine.
  `MapResY` is absent from every label table (recon/03 §3). — verified (data), inferred (role)
- **Minimap texture semantics** — a full scan of `chitin.key` (26 BIFs, 25,836
  resources), the 12 global RIMs, `patch.erf`, and all four `TexturePacks/*.erf` finds
  **no** `<area>`-named TGA/TPC (e.g. no `m01aa` texture anywhere; `m01aa` exists only as
  LYT 3000 + VIS 3001). **Verified from data alone:** this install ships no per-area
  minimap bitmap; the map is rasterized from the ARE `Map` affine over area geometry.
  Which engine pass does that rasterization is binary-side → **marked for lane s1-07**.
- The remaining recon/12 opens (live dialogue-camera object field, supermodel visual
  chain, camera mode `+0x38` live observation) are runtime items → lane s1-06, unchanged.

## 7. Reproduce

```bash
python tools/tests/test_mapcompile.py            # synthetic: 13 tests
KOTOR_GAME_DIR=E:/SteamLibrary/steamapps/common/swkotor \
  python tools/mapcompile/compile.py --out artifacts/lanes/s1-03-mapcompile   # corpus, exit 1 iff hard entries
python tools/mapcompile/compile.py --module end_m01aa --out <tmp>              # single area, exit 0/1
```

Exit codes: 0 = no hard entries; 1 = at least one hard-fail entry; 2/abort on install
hash drift (sentinel check before any read).
