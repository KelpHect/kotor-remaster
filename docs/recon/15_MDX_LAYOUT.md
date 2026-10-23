# MDX layout and full-corpus coverage (lane 10 continuation)

Status: **complete for the shipped corpus.** Every MDX byte reachable from `chitin.key` and the
module archives is assigned to a decoded field, sentinel, or allocator pad. Three semantic
questions remain and are explicitly labeled; none is a byte-accounting gap.

## Coverage

| metric | value |
|---|---:|
| chitin.key MDL/MDX pairs | **2,832 / 2,832** (0 parse errors) |
| additional unique pairs in RIM/ERF | 0 (212 resrefs all already in KEY) |
| MDL bytes / MDX bytes | 477,561,259 / 489,549,652 |
| MDX payload / sentinel / pad | 486,271,196 / 2,886,776 / 391,680 |
| payload + sentinel + pad | **489,549,652 (exact)** |
| mesh nodes / valid blocks | 76,782 / 76,703 |
| vertices / MDX-backed faces | 11,648,777 / 5,718,604 |
| empty/procedural meshes | 79 (64 saber + 15 placeholders) |
| files with accounting mismatch | **0** |

The 528-file sample is explained by the same rules; full counts are 5.4× the sample. Every block is
contiguous, the first starts at 0, and the last tail ends exactly at EOF.

## Layout

```
block      = vc * stride payload
           + one stride-byte sentinel
           + 0..12 bytes allocator pad
next block = end of previous pad (== next mesh header's +324)
```

* Sentinel: static `1e7,1e7,1e7` then zeros; skin `1e6,1e6,1e6`, zeros, `1.0` in the weights slot,
  then zeros. 0 bad sentinels / 76,703. Pad is allocator slack (0/4/8/12 B); 15,666 blocks carry
  nonzero stale bytes, the only bytes without a semantic invariant.
* **Stride = 24 + 8·textureCount + 32·skin + 36·TBN**, with `textureCount = mesh+304 >> 16`;
  verified for all 76,703 blocks (79 empty/procedural special cases).

| stride | count | fields |
|---:|---:|---|
| 24 | 13,467 | pos, normal |
| 32 | 20,419 | + uv0 |
| 40 | 37,011 | + uv0, uv1 |
| 60 | 23 | + T,B,N (default basis) |
| 64 | 892 | skin: uv0, weights 4f, bone ids 4f |
| 68 | 286 | + uv0, T,B,N |
| 76 | 4,529 | + uv0, uv1, T,B,N |
| 100 | 76 | skin: uv0, T,B,N, weights, bone ids |

* 968 skin blocks (B = 1..17), 386,120 vertices. `mesh+344/+348` are MDX offsets; `+352` is a
  node→bone f32 map (node-number order, −1 unused); `+356` node count; `+360` 16 B/node qbone;
  `+372` 12 B/node tbone; `+384` 4 B/node aux4; `+396` u16[B] node-number bone list. Weights sum to
  1 and bone ids are integral; 0 bad weights/bones. A `c_bantha` vertex was reconstructed 64/64
  bytes and resolved to `BTShoulders`/`BTRFUpperleg`.
* **Engine identities (verified):** `0x46a2f0` binds `"qbone_ref_inv"`→data-header `+0x1b8`
  (16 B), `"tbone_ref_inv"`→`+0x1c4` (12 B), `"boneconstantindices"`→`+0x1d0` (4 B). File-header
  words are offsets relative to byte 12; runtime `+0x1b8/+0x1c4/+0x1d0` are heap pointers with
  used/capacity companions. CPU skin `0x487570` uses the inline bone list + node2bone map, while
  `0x48a0d0` and palettes `0x48a520/0x48c650/0x48d040` consume qbone/tbone (T += R(Q)·tbone,
  Q *= qbone). A secondary u16 bone-id index path exists at `0x487b04`; its reachability is open.
  The GPU bone-constant consumer and relocation routine are not yet identified; do not call aux4
  unused.
* Groups: `+176` counts (= fc·3), `+188` offsets, `+200` runtime pool handles; the old “unknown
  second u32” is handle[0] (`handles_ptr == offsets_ptr + 4` in 76,703/76,703). Group count is
  **1 in every shipped mesh**; multi-group addressing has no shipped data.

## Exception classification

* **Bbox**: 76,595/76,703 = min/max(vertices ∪ origin) within 1e-5. The 108 exceptions are a
  one-sided expansion by a vector T (0.004–87.7 m) that matches no node/parent/world/animation
  transform; it is exporter/scene-time data. Exact offsets/sizes/values are recorded; the source is
  **open/irreducible from the file**.
* **Winding**: 389 faces / 19 blocks across mirrored door/panel/track surfaces, wing membranes, the
  `c_notready` test asset, hoses, turret/loft pieces, droid hands, an effect quad, one room floor,
  and a dev cone. Consistent with mirrored/double-sided surfaces; render side is not decidable
  statically.
* **Plane residual**: 13,190 faces / 40 blocks. 20 blocks (504 faces) are float32 precision at
  2,244–10,025 m; 20 blocks (12,686 faces) are stale/deformed face records (normals still aligned,
  plane constant does not match the only stored position set). Irreducible from the shipped bytes.
* **Zero normals**: 13,018 verts / 3,083 blocks; 87.3% on foliage/billboard textures; 155 are
  unreferenced. No fallback rule is stored; unlit/alpha/collision surfaces is inferred.
* Degenerate 40/15; 12 UV sentinels `0xFEFFEEEE`; 64 saber + 15 empty placeholders.

## Remaining semantic questions

1. **GPU skin consumer / relocation** — CPU qbone/tbone paths and `aux4 =
   boneconstantindices` are verified; the GPU bone-constant consumer, the file→runtime relocation
   routine, and the secondary u16 index path's reachability remain open.
2. **Multi-group addressing** — no shipped mesh has group count > 1; a synthetic asset would be
   needed. The engine loop is structurally N-group capable.
3. **Bbox expansion source** — exporter/scene-time provenance; values are known, origin is not in the
   shipped file.

## Evidence

`artifacts/lanes/10-coords-open/cont/mdx2/` (untracked): `summary.md`, `mdx2_mdl.py`, `full_scan.py`,
`final_stats.py`, `full/full_scan.json`, `full/final_stats.json`, `coverage_v2.json`, probes.
Earlier sample: `cont/mdx/` (634 blocks, 14/14 WOK walkmesh match).
