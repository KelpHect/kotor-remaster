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
- The source Odyssey conventions (reported Z-up right-handed; exact handedness, units, quaternion
  order, and matrix layout still to be verified) are converted exactly once in the asset/map
  importer. Gameplay and renderer code never apply axis flips.
- Every converted asset stores the source resource hash and the converter/version that produced it.
  A transform or convention change forces a cache rebuild.
- The specific source→canonical mapping is provisional until the coordinate probe validates it
  against known world landmarks; the runtime consumes a matrix, not hardcoded flips.

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

1. GFF wire semantics are verified; source handedness, unit scale, quaternion order, and
   Bearing/XOrientation conventions remain open (+Z-up is supported by the probe).
2. MDL node/matrix layout, bind-pose and supermodel composition, animation quaternion order.
3. BWM/WOK vertex coordinate space and winding; LYT room origin semantics; PTH node space.
4. Camera FOV/aspect/near/far conventions and DLG/GIT camera transforms.
5. Vulkan projection/viewport Y-flip and depth-range handling; confirm with a known screen landmark.
6. Current FSR SDK version/license and RT capability baseline for the target hardware tiers.

The coordinate probe and its fixtures are the first deliverable of lane L5 in this plan revision.
