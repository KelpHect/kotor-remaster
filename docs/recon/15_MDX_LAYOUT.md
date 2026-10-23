# MDX vertex/index layout (lane 10 continuation)

Status: **verified on shipped bytes.** The MDX block layout, vertex strides, face records, duplicate
index buffer, bbox rule, and walkmesh correspondence are decoded. A few data quirks remain open.
Static parsing only: no game run, no Ghidra, no install writes.

## Layout

MDX has **no file header**. Each mesh/walkmesh block is addressed by the MDL mesh header's `+324`
u32 (byte offset into the MDX). Blocks appear in ascending `+324` order, first at 0.

```
payload = vertex_count(+304 & 0xFFFF) * stride(+252) bytes
vertex  = position f32[3] @ +0
          normal   f32[3] @ +12
          uv0      f32[2] @ +24   (stride 32 and 40)
          uv1      f32[2] @ +32   (stride 40 only)
tail    = one sentinel record of `stride` bytes (f32 1e7,1e7,1e7, then zeros)
block   = align16(payload + stride); next block starts there
```

Strides observed: 32 B (position, normal, uv0), 40 B (adds uv1/lightmap), 24 B (position, normal
only). One empty `stride=0xFFFFFFFF, count=0` placeholder exists in `m01aa_03a`.

Faces live in the MDL, not the MDX. Each mesh header has `+8` (offset relative to file+12) and
`+12` (count) for 32-byte face records:

| offset | type | meaning |
|---|---|---|
| +0 | 3f | unit face normal |
| +12 | f32 | negated plane distance (`n·v0 + value = 0`) |
| +16 | u32 | material id |
| +20 | 3×u16 | adjacency face indices (`0xFFFF` = none) |
| +26 | 3×u16 | vertex indices into the MDX block |

The `+188` descriptor points to a second, compact index buffer: its first u32 locates
`descriptor+8`, which holds `face_count` triples of u16. In **634/634** sampled meshes this buffer is
byte-identical in order to the face records' indices. The descriptor's second u32 is unexplained.
There is no index block in the MDX itself.

## Verified checks

Sample: 15 `m01aa_*` rooms plus `mydoor`, `plc_chair2`, `plc_metalbox`, `c_drdastro`, `pmbam`,
`n_commm` — **634 mesh blocks, 182,150 vertices, 84,707 faces**. MDX sizes match the MDL field at
`+188` in every file.

* Block addressing accounts for every byte in 19/21 files (`pmbam`/`n_commm` have trailing data).
* Every face index is below the vertex count and max index = count−1; zero degenerate triangles.
* Face plane `n·v0 + value < 3e-7` on 632/634 blocks (the two exceptions are `mydoor` panels).
* Bbox equals vertex min/max plus the node-local origin; 633/634 match to < 2e-5.
* Winding: 84,638/84,707 faces have `cross(v1−v0,v2−v0)·normal > 0`; all rooms, chair, metalbox, and
  droid are 100% positive (RH, CCW front).
* Walkmesh (node type 545) uses the same layout and is node-local model space. For 14/14 rooms with
  a 545 node, the MDX vertex set matches the WOK set 1:1 (unique counts equal); max nearest distance
  is 8.3e-5 m for `01a` and ≤ 4.90 mm for all 14 (12/14 < 1 mm). World placement is
  `LYT origin + node chain`, with no WOK `position` term.

## Inferred

* Positions/normals are node-local; the node chain supplies world placement.
* `uv0` is the diffuse UV (tiles outside [0,1]); `uv1` is the lightmap UV (0.0039–0.9961).
* The 1e7 sentinel plus 16-byte alignment is an exporter allocation artifact.
* `+188` is a per-vertex-group descriptor; count is 1 in every sampled static mesh, so multi-group
  addressing is untested.

## Open

1. `pmbam` (75,937 B) and `n_commm` (66,913 B) have MDX data after the last addressed block; all
   trimesh headers are accounted for, so it is likely LOD/skin/animation payload, not a missed node.
2. `mydoor` two 256-face panels (ni 2/8) have 37/32 reversed faces and a plane residual up to
   1.186 m; likely a mirrored/animated pair, rendering side not decidable statically.
3. `m01aa_06a` ni 11 (`Object978`) has a header bbox larger than its 64 vertices; whether it is a
   union/skin bound is unknown.
4. 74/182,150 vertex normals are zero (rooms plus `mydoor`), presumably unlit/alpha surfaces; no
   fallback rule is specified.
5. Multi-group descriptor layout (`+184`/`+188`, count > 1) is untested; the second u32 is unexplained.
6. `m01aa_07b` has no 545 walkmesh node, so its WOK could not be cross-checked.

## Evidence

`artifacts/lanes/10-coords-open/cont/mdx/` (untracked): `summary.md`, `mdx_mdl.py` (clean-room
reader/validator), `run_sample.py`, `mdx_report.json`, annotated dumps, and read-only MDX copies.
