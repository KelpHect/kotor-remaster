# Recon 24 — First textured tools-side viewer (lane s1-08)

Status: **complete for the Sprint 1 gate G1 item 8**. `tools/viewer/` is a
self-contained pure-Python (stdlib only) viewer that reads the user's install
**read-only** and renders areas (top-down + perspective, TPC textures on
walkmesh/room surfaces) and `.gui` screens (control layout) to pure-Python
PNG (zlib+struct) plus JSON scene summaries with self-checks.

Build under test (read-only hash sentinel, re-verified at every run, stops on
drift): `swkotor.exe` SHA-256
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6` — **verified**
(match confirmed by `tools/viewer/kotor.py::check_install_sentinel`).

## What was built (new files only; no existing tracked file touched)

```
tools/viewer/
  __init__.py        package doc, REPO_ROOT, game_dir() (Git Bash /e/ -> E:/), BUILD_SHA256
  pngwrite.py        Image (RGB/RGBA) + PNG encoder (zlib+struct, filter 0)
  raster.py          software rasterizer: flat/textured tris, z-buffer, blit, lines
  tpc.py             TPC decoder (DXT1/3/5, RGB, RGBA, gray, BGRA, top mip) + TGA decoder
  lyt.py             LYT (#MAXLAYOUT) parser: rooms, door hooks
  wok.py             BWM V1.0 (WOK/DWK/PWK) parser: verts/faces/normals/edges
  mdl.py             MDL+MDX parser: node tree, trimesh faces/verts/UVs, WXYZ transforms
  kotor.py           read-only VFS-lite over KOTOR_GAME_DIR (imports tools/resources/
                     kotor_archive.py read-only); install sentinel check
  gffx.py            gff.Gff from in-memory bytes (imports tools/formats/gff.py read-only)
  area.py            area scene loader (module RIM ARE/GIT/IFO/PTH + LYT/WOK/room MDL/MDX)
  gui.py             .gui GFF parser: flat CONTROLS tree + texture resrefs
  area_viewer.py     CLI (a): renders <area>_topdown.png, <area>_perspective.png, JSON
  gui_viewer.py      CLI (b): renders <gui>.png + <gui>_gui.json
tools/tests/test_viewer.py   19 synthetic-fixture tests (no game data)
artifacts/lanes/s1-08-viewer/   all renders + JSON summaries (gitignored)
```

`tools/formats/gff.py` and `tools/resources/kotor_archive.py` are imported
read-only; `tools/geometry` and `tools/formats` were not edited.

## Usage

```bash
cd /e/Projects/kotor-remaster/kotor-remaster
export KOTOR_GAME_DIR=/e/SteamLibrary/steamapps/common/swkotor   # read-only
python tools/viewer/area_viewer.py --out artifacts/lanes/s1-08-viewer \
    --modules end_m01aa tar_m09ab          # defaults
python tools/viewer/gui_viewer.py --out artifacts/lanes/s1-08-viewer \
    --guis mainmenu maininterface          # defaults, scale 2 -> 1280x960
python -m unittest tools.tests.test_viewer      # 19 tests, synthetic only
python tools/tests/run_all.py                   # full suite incl. this lane
```

## Data path (verified against this install by direct probing)

- `modules/end_m01aa.rim` holds `m01aa` ARE (2012) + GIT (2023) and `module`
  IFO (2014); `end_m01aa_s.rim` holds the PTH (3003). **Verified** (RIM entry
  dump in this session).
- LYT (3000)/VIS (3001)/WOK (2016)/MDL (2002)/MDX (3008) for the area and its
  rooms live in the base BIFs via `chitin.key` (`layouts.bif`, `models.bif`).
  **Verified** (KEY lookups: `m01aa` LYT @ bif 3, `m01aa_01a` WOK/MDL/MDX @
  bif 18).
- Base (non-lightmap) textures exist **only** as TPC (3007) in
  `TexturePacks/swpc_tex_tpa.erf` (e.g. `lhr_wall01`); lightmaps are TGA (3)
  in `lightmaps*.bif` (e.g. `m01aa_01a_lm1` @ bif 5). **Verified** by lookup
  probes. GUI textures are TPC in `swpc_tex_gui.erf` / `patch.erf`.
- TPC header (verified by probing all 4 texture packs + patch.erf, 11,452
  files): 128-byte header, u32 data_size @0 (top mip; 0 = raw), f32 alpha_test
  @4, u16 w/h @8/@A, u8 encoding @0x0C (1 gray, 2 RGB/DXT1, 4 RGBA/DXT5, 0xC
  BGRA), u8 mip count @0x0D, top mip at byte 128, TXI text appended. Matches
  `docs/recon/02` §2. RGB-vs-DXT disambiguation by data_size (w*h*3 vs
  ceil-blocks*8/16). **Verified** (ratio scan: enc 2 -> 0.5 B/px = DXT1,
  enc 4 -> 1.0 B/px = DXT5; mips = log2(max(w,h))+1).
- MDL mesh header offsets (node+80): face_off @+8, face_count @+12, tex0[32]
  @+88, tex1[32] @+120, stride @+252, vc|tc<<16 @+304, MDX block @+324; face
  record 32 B with 3×u16 indices @+26; MDX vertex stride 24/32/40 (+uv0 @24,
  uv1 @32). **Verified** against `m01aa_01a.mdl` (78 nodes, 60 trimeshes
  parsed; door-node positions match LYT hooks per `docs/recon/04`).
- GUI GFF: root `CONTROLS` list; each control has `EXTENT` {LEFT,TOP,WIDTH,
  HEIGHT} in 640×480 space, `BORDER`/`HILIGHT` {CORNER,EDGE,FILL} resrefs,
  `TEXT.FONT`, type-specific structs. **Verified** on `mainmenu`/`maininterface`
  (gui.bif via KEY).
- Texture lookup order used by the viewer: module RIMs → `patch.erf` →
  `swpc_tex_gui.erf` → `swpc_tex_tpa.erf` → KEY/BIF TGA. **Verified** against the
  documented five-pass VFS and the measured quality mapping
  (`docs/recon/08`/`10`: quality 2/1/0 → tpa/tpb/tpc; gui pack always loaded).
  The viewer pins tpa + gui, which cover the sampled resrefs.

## Sample outputs (this run, all checks green)

| output | size | checks |
|---|---|---|
| `m01aa_topdown.png` (1024×768) + `m01aa_perspective.png` (800×600) + `m01aa_scene.json` | 72,984 / 563,660 B | **42/42** |
| `m09ab_topdown.png` + `m09ab_perspective.png` + `m09ab_scene.json` (module `tar_m09ab`) | 47,867 / 336,448 B | **26/26** |
| `mainmenu.png` (1280×960) + `mainmenu_gui.json` | 129,912 B | **9/9** |
| `maininterface.png` (1280×960) + `maininterface_gui.json` | 36,469 B | **31/31** |

Counts cross-checked against `docs/recon/11` (verified): m01aa = 15 LYT rooms,
15 hooks, 850 WOK vertices, **1,365 WOK triangles**, 858 upward floor
triangles, GIT 26 creatures / 15 doors / 58 waypoints / 60 placeables — all
exact matches in `m01aa_scene.json`. m09ab: 3 rooms, 142 WOK tris, 2,262 MDL
tris. mainmenu: 13 controls (6 image/label, 6 button, 1 listbox), 32 texture
refs; maininterface: 102 controls, 209 texture refs.

Self-checks printed to stdout and stored in each JSON: GFF structural checks
(ARE/GIT/IFO/PTH), LYT room count vs loaded rooms, top-down floor-triangle
count vs WOK face records, perspective MDL triangle count vs sum of mesh
face counts, per-texture decoded dimensions vs TPC/TGA header dimensions, GUI
rendered control count vs the GFF `CONTROLS` list count. Sample lines:

```
[m01aa] rooms=15 wok_tris=1365 floor_tris=858 mdl_tris=78346 textures=9/9 checks=42/42
[m09ab] rooms=3 wok_tris=142 floor_tris=46 mdl_tris=2262 textures=3/3 checks=26/26
[mainmenu] controls=13 texture_refs=32 checks=9/9
[maininterface] controls=102 texture_refs=209 checks=31/31
```

## Tests

`python -m unittest tools.tests.test_viewer` → **19 tests, all pass**
(synthetic fixtures only: hand-built TPC DXT1/DXT5/RGBA/gray/RGB, TGA 24-bit
+ RLE, LYT text, BWM WOK, a minimal MDL+MDX with transform check, a synthetic
GUI GFF via the project writer, PNG encode, rasterizer z-buffer/alpha/blit,
path normalization). Full repo suite `python tools/tests/run_all.py` →
**87 run, 80 passed, 0 failed, 7 skipped** (pre-existing skips; 19 of the 87
are this lane's).

## Limitations (software raster, no shading parity)

- Affine texture mapping (no perspective-correct UVs), no lighting/shading,
  no lightmap blending, no alpha-test against the TPC alpha_test value —
  colors are sampled directly. No shading parity with the engine is claimed
  (P4/P5 scope).
- Top-down walkmesh UVs are synthetic (world XY × 8 px), not stored UVs —
  WOK has no UVs; the room's TPC texture is sampled by world position.
- Perspective camera is fixed at the IFO entry (eye = entry + 1.6 m, looking
  along `Mod_Entry_Dir`), fov 60°, z-buffered, both faces drawn (no culling).
- Only mip 0 of TPCs is decoded; cubemap TPCs are counted but not rendered;
  skin/anim/dangly MDL nodes are parsed for structure but not skinned.
- GUI rendering draws EXTENT rects, FILL/EDGE/CORNER textures stretched, and
  flat COLOR fills; **text glyphs are not rasterized** (no font renderer),
  HILIGHT state and MOVETO are ignored. Stacking = CONTROLS list order.
- `TexturePacks` quality selection (tpa/tpb/tpc) is not modeled; the viewer
  pins gui + tpa packs (open item from `docs/recon/10`).

## Evidence status

- verified: install sentinel hash; RIM/KEY/BIF/ERF placement of every
  resource type used; TPC/TGA/MDL/WOK/LYT/GUI layouts and all counts quoted
  above; all self-check counts (stored in the JSONs).
- inferred: synthetic top-down UVs; perspective camera placement from IFO entry.
- open: font rasterization; cubemap rendering; shading/lightmap parity (P4/P5).
  Texture-pack quality→archive mapping is now measured (quality 2/1/0 → tpa/tpb/tpc;
  docs/recon/08/10), so the viewer's gui+tpa pin is a deliberate subset, not a gap.
