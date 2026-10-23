# ADR 0002 — Coordinate contract, map compilation, and render-feature strategy

Status: accepted for planning (2026-10-23)
Related: ADR 0001, master plan, L5 (areas/navigation), L6 (renderer), asset pipeline.

## Context

KotOR's data is expressed in the original Odyssey engine's conventions: GFF vectors/orientations,
MDL/MDX node transforms and animation tracks, BWM/WOK walkmeshes, LYT/VIS room layout, PTH path
graph, cameras, and UI coordinates. The new engine uses its own canonical space and Vulkan's
clip-space/depth conventions. Converting at multiple layers, or copying numbers between engines
without a single contract, produces inverted maps, mirrored geometry, wrong headings, bad pivots,
broken skinning, and misaligned lightmaps.

A prior port of another large game to a modern engine failed with misplaced walls because source
conventions and the target engine's conventions were not pinned down before the map pipeline was
built. The lesson is explicit: do not rely on a single global axis fix or an engine importer's
guess; decode each transform source, convert at one boundary, and validate against the original.

The renderer enhancements the project wants (re-baked/dynamic shadows, ray tracing, FSR-class
upscaling) are independent renderer features. Vulkan 1.4 is the API foundation, but ray tracing
requires the Vulkan ray-tracing extensions and capable hardware, and FSR is an AMD SDK integration.
They are implemented in the Remaster path, never assumed from the API version, and never allowed to
change simulation state.

## Decision

### 1. One canonical space, one conversion boundary

- The engine's canonical space is **right-handed, Y-up, meters**, compatible with glTF and modern
  content tools.
- The source space is verified as **right-handed Z-up, 1 unit = 1 m** on a populated area
  (WOK winding/normals, humanoid height, room geometry). Quaternion order is W,X,Y,Z. Source
  transforms are converted exactly once in the asset/map importer; gameplay and renderer code
  never apply axis flips.
- Every converted asset stores the source resource hash and the converter/version that produced it.
  A transform or convention change forces a cache rebuild.
- Remaining orientation details (Bearing sign/zero, camera Height, door appearance final facing,
  MDX block layout, animation/supermodel overrides) are open and must be closed by fixtures before
  the canonical conversion is frozen. The runtime consumes a matrix, not hardcoded flips.

### 2. What must be decoded and validated

- **GFF:** position and orientation fields; field types 16/17/18 semantics; quaternion component
  order; object placement, cameras, triggers, transitions.
- **MDL/MDX:** node hierarchy and pivots, bind poses, supermodels, animation tracks, quaternion
  multiplication order, skinning weights, scale, lights/emitters, hooks.
- **BWM/WOK/DWK/PWK:** vertex space, triangle winding, normals, surface IDs, adjacency, AABB;
  area vs door vs placeable walkmeshes.
- **LYT/VIS/PTH:** room origins/orientations, visibility graph, path nodes/connections; door and
  placeable connectors.
- **Area/IFO/GIT:** entry points, headings, camera styles, placeable/creature transforms.
- **Cameras/UI:** FOV (vertical/horizontal), aspect, near/far, projection conventions, screen-space
  coordinates, GUI scaling.
- **Vulkan specifics:** clip-space Y direction, depth range [0,1] vs GL [-1,1], front-face winding,
  viewport origin. These are handled in the RHI/projection layer, not by flipping assets.

### 3. Map-compilation contract

- Offline map compilation produces scene geometry, room/visibility graph, collision/walkmesh,
  pathfinding graph, spawn/transition anchors, lightmaps, and metadata from original data.
- The compiler validates:
  - room bounds and overlap; VIS references exist;
  - triangles are non-degenerate, winding/normals consistent, surface IDs valid;
  - walkmesh/path nodes connect where the original does; doors/transitions land inside bounds;
  - spawn points/headings are inside walkable space;
  - lightmap UVs and material assignments are in range.
- Failures are hard errors, not warnings. A map is not "compiled" until its validation report is
  clean.
- Debug overlays render room bounds, portals/visibility, walkmesh surface IDs, path nodes, and
  connectors for side-by-side comparison with the original.

### 4. Verification

- Landmark fixtures: known NPCs, doors, waypoints, spawn points, and cameras from shipped GFFs;
  compare position/heading/scale against runtime traces from the original.
- Transform unit tests: quaternion order, parent/supermodel composition, bind-pose round-trip,
  world↔local, handedness/winding, UV orientation.
- Walkmesh/path tests: point containment, raycasts, door traversal, path distances.
- Projection tests: Vulkan/viewport Y-flip and depth range; known screen-space landmarks.
- Visual gates: fixed camera/tick captures in Classic mode, SSIM/pixel tolerance; any mirrored or
  offset frame fails the gate.
- Map validation is a release gate for every area (P1/P4/P6).

### 4.1 Probe results (m12ab, 2026-10-23)

- GFF wire semantics are verified on a real GIT/ARE pair: section lengths, struct/field record
  layout, type-first fields, indirect field rows, and list records (`u32 count`, `u32 struct
  indices[]`). Details and invariants: `docs/recon/03_GFF_COORDINATES.md`.
- Z is vertical in this area: ten placeables lie within `|Z| <= 0.013` while X/Y span about
  8.5/7.8 units; a sound emitter sits at Z ~0.991. +Z-up is supported by this sample.
- Not proven: world handedness, unit scale, Bearing units/sign, and creature
  XOrientation/YOrientation. The sample has no creatures/doors/cameras, all bearings are `-0.0`,
  the ARE map calibration is zero, and entry points live in IFO. No conversion is frozen.
- Next probes: IFO entry points, a populated module for creature/door orientations, LYT/VIS/WOK/PTH,
  and MDL node transforms.

### 4.2 Verified geometry contract (m01aa / Endar Spire)

- **Source space:** right-handed, Z-up, 1 unit = 1 m. WOK faces pass `cross(v1-v0,v2-v0) · normal > 0`
  for 1365/1365 faces; humanoid heights calibrate to 1.581/1.585 m.
- **IFO entry:** X/Y/Z is a ground/feet point; `Mod_Entry_Dir_X/Y` is a unit 2D facing vector
  (117/117 norms 1.0), with no Dir_Z. Entry area matches an ARE.
- **GIT orientation:** creature `XOrientation/YOrientation` is a unit forward vector (26/26);
  door/placeable `Bearing` is a scalar yaw in radians, matching the wall/hook plane. Use the
  instance Bearing, not the LYT hook's facing. Door Z sits below the WOK floor; do not snap it.
  The right-handed, +Y-forward interpretation is inferred from asymmetric chair placement; the
  sign/zero convention still needs an asymmetric oracle.
- **Area geometry:** LYT room origin is the MDL model origin; WOK vertices are already world-space
  (do not add LYT origin or `WOK.position`); PTH points are 2D world space with Z from WOK.
- **MDL:** quaternion order is W,X,Y,Z; world = room origin × parent→child node transforms with no
  scale. The model-header bounding box is a dummy; trimesh bounds are node-local. MDX vertex/face
  block layout is not yet decoded.
- **GFF:** the single-field struct rule is required (`field_count == 1` → direct field row);
  the public parser implements it and validates IFO/GIT/ARE.
- Details and 12 fixture checks: `docs/recon/04_GEOMETRY_COORDINATES.md`.

### 5. Render features and modes

- **Classic (1:1 oracle):** emulates the original fixed-function pipeline, original lightmaps,
  stencil shadow volumes, alpha test/blend, fog, and GUI. No new lighting is allowed to change the
  1:1 comparison.
- **Remaster (optional):** same simulation and assets, enhanced presentation:
  - static lightmap/AO re-bake from original geometry and materials;
  - dynamic shadow maps (CSM) for characters/objects, or optional ray-traced shadows/reflections;
  - high-res texture packs, anisotropic filtering, MSAA/SSAA, HDR/tonemapping, SSAO, bloom;
  - widescreen/HFR, FSR/DLSS/XeSS-class upscaling with depth/motion vectors;
  - optional PBR approximations and improved materials, explicitly non-canonical.
- **Vulkan 1.4:** primary API; RT uses `VK_KHR_ray_query`/`ray_tracing_pipeline` when supported,
  with raster fallback. FSR is integrated as a pinned AMD FidelityFX SDK feature with proper
  jitter/motion-vector plumbing; no FSR/SDK version is assumed before implementation. No engine
  feature may require RT or upscaling to be playable.
- Remaster and Classic share scene/asset data and simulation; switching modes cannot alter saves,
  collision, scripts, or gameplay outcomes.

## Consequences

- Coordinate/transform work moves earlier (P1), with a dedicated probe and validation suite.
- The asset cache and map compiler need a versioned transform contract and per-asset provenance.
- Renderer milestones split into Classic parity first, then Remaster enhancements, so visual
  upgrades cannot mask fidelity bugs.
- RT/FSR support is hardware- and SDK-dependent; quality tiers and fallbacks are part of the design,
  not an afterthought.

## Open questions (must be closed by evidence)

1. Bearing sign/zero is resolved: radian yaw, no zero sentinel, same sign as the LYT hook yaw.
   Handedness is **RH**, verified in engine code (`0x50a0e0`/`0x50a7b0` load `Bearing`; `0x4ac960`
   builds `q=(cos B/2,0,0,sin B/2)`; `0x5853c0`/`0x5896e0` produce `forward=(−sin B, cos B)`) and
   independently by the PWK use-point oracle (10 decisive, 0 LH).
   Doors select their model by UTD `GenericType` → `genericdoors.2da`; `Appearance` is not the
   selector and adds no yaw. `WOK.position` is the room walkmesh node (type 545) local translation
   (453/453 bit-exact), not a placement term; it is a pivot for DWK/PWK. Camera `Position` is a
   ground point with `eye = Position.z + Height` and
   `forward = R_yaw·(0, sin Pitch, −cos Pitch)`. The ARE minimap two-point affine is decoded and
   DLG camera models resolve; `CameraAnimation`/`CameraAngle` composition remains open.
2. Animation payloads are decoded: type 8 position is additive to bind, type 20 orientation is
   absolute, there is no NaN sentinel (0x7fdffbff is identity). Supermodel inheritance is verified
   for disjoint clips; nearest override order and inherited-clip name-vs-index remap remain open.
   The MDX block layout is decoded (`docs/recon/15`); trailing LOD/skin data and multi-group
   descriptors are in a closing pass.
3. Camera FOV/pitch are degrees and the GIT view basis is verified; aspect/near/far are absent
   (engine defaults). Vulkan projection/viewport Y-flip and depth range still need a rendered
   landmark in the new renderer.
4. Current FSR SDK version/license and RT capability baseline for the target hardware tiers.

The initial coordinate probe and 12 fixtures are delivered; mapping can start against the verified
contract, but the open items above remain gates before freezing the cache/converter.
