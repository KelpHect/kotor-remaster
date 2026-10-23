# KotOR 1 Interoperability Runtime (unofficial)

Status: **research and planning** — there is no playable engine build yet. This repository contains
the clean-room plan, reverse-engineering methodology, evidence tooling, and architecture decisions
for a new, data-driven runtime for Star Wars: Knights of the Old Republic (2003).

The runtime is intended to load a user-supplied KotOR 1.0.3.0 installation and run it in place. It
does not ship, download, extract, or redistribute game assets, and it does not require or execute
the original game executable.

Target architecture: original C++23 runtime, SDL3 platform, Vulkan 1.4 primary RHI with an OpenGL
4.5 reference backend; a Classic path for 1:1 behavior and an optional Remaster presentation path.

## Verified progress

- Fresh Ghidra 12.1.4 analysis of a user-owned 1.0.3.0 build.
- Full pseudocode pass for the initial function inventory: 8,616/8,616 decompiled with zero failures.
- Script action table: **772/772 actions mapped** and **565/565 handler targets named**; 440
  handler functions + dispatcher were added and decompiled in an isolated project copy, then the
  last 125 `FUN_` handlers were renamed (9,057 internal functions; 350 external imports).
- Archive inventory: 25,836 base BIF resources, 20,575 module RIM resources, 1,936 global-RIM
  resources, plus patch/texture-pack ERFs; 49,265 TLK strings.
- TLK V3.0 parser validated; SQLite evidence ledger; action-table extraction tooling.
- Full install manifest: 15,380 files / 3.36 GiB hashed without modifying the install.
- 2DA parser validates all 209 shipped tables (7,750 rows, 111,521 cells, zero failures).
- GFF V3.2 wire semantics verified; the single-field struct rule is implemented and IFO/GIT/ARE
  validate. Coordinate probes on m01aa establish RH Z-up, 1 unit = 1 m, IFO ground-point/unit-dir,
  creature forward vectors, radian Bearing, world-space WOK, and the LYT/MDL transform chain;
  Bearing sign, MDX blocks, camera height, and animation overrides remain open
  (`docs/recon/03`, `docs/recon/04`).
- GFF corpus validated: 11,143/11,143 shipped payloads parse, 1,854,982/1,854,982 values resolve,
  zero intrinsic failures; the struct-slot packing invariant was disproved by shipped files and
  replaced with disjoint-range checks (`docs/recon/07`, 30 regression tests).
- Script corpus: 8,666 unique NCS (13,139 occurrences) all decode; 285,015 ACTION instructions
  reference only known action IDs; 466/772 actions are used. 5,262/8,666 scripts round-trip through
  the DeNCS lineage decompiler and compiler (`docs/recon/09`).
- Save/VFS: save container parser/validator and structural writer, archive readers/writers, and a
  five-pass resolver, with 33 synthetic/real-install tests. Dynamic hooks confirm the pass order,
  Override precedence, and quality 2/1/0 → tpa/tpb/tpc (`docs/recon/08`, `docs/recon/10`).
- Engine scaffold: C++23 + SDL3 + Vulkan 1.4 (1.3 floor) + OpenGL 4.5 + ImGui builds from
  `engine/` with `/W4 /WX`; Vulkan and OpenGL both run 120 frames, headless CTest 2/2. This is a
  clear/ImGui presentation scaffold, not a renderer yet (`engine/docs/BUILD.md`).
- m01aa geometry path: LYT/WOK/MDL/PTH/IFO/GIT parse into a top-down preview with 1,365 WOK
  triangles, 15 doors/hooks, 77 PTH nodes, and 45/45 cross-checks (`docs/recon/11`).
- Architecture decisions for the runtime/renderer, coordinate + map-compilation contract, and the
  user-supplied-install distribution model.

## Repository layout

```
engine/      C++23 runtime scaffold: SDL3, Vulkan 1.4 / OpenGL 4.5, ImGui
docs/        master plan, reconnaissance, ADRs, format/VM notes
tools/       read-only inventory/parsers, Ghidra exporters, action-table and evidence tooling
artifacts/   local only (gitignored): Ghidra projects, exports, caches, captured evidence
```

Start with:

- [`engine/docs/BUILD.md`](engine/docs/BUILD.md) — build the engine scaffold and run its smoke tests
- [`docs/MASTER_PLAN.md`](docs/MASTER_PLAN.md) — program lanes, phases, gates, verification
- [`docs/SPRINT0.md`](docs/SPRINT0.md) — current foundation sprint exit criteria and lane board
- [`docs/recon/01_ARTIFACTS.md`](docs/recon/01_ARTIFACTS.md) — verified artifact/content inventory
- [`docs/recon/02_TECHNICAL_NOTES.md`](docs/recon/02_TECHNICAL_NOTES.md) — format, VM, VFS, rules notes
- [`docs/recon/03_GFF_COORDINATES.md`](docs/recon/03_GFF_COORDINATES.md) — verified GFF wire semantics and coordinate evidence
- [`docs/recon/04_GEOMETRY_COORDINATES.md`](docs/recon/04_GEOMETRY_COORDINATES.md) — IFO/WOK/LYT/MDL coordinate contract and fixtures
- [`docs/recon/05_2DA.md`](docs/recon/05_2DA.md) — 209-table 2DA coverage
- [`docs/recon/06_MANIFEST.md`](docs/recon/06_MANIFEST.md) — complete install manifest summary
- [`docs/recon/07_GFF_CORPUS.md`](docs/recon/07_GFF_CORPUS.md) — all-resource GFF validation and corrected invariants
- [`docs/recon/08_VFS_DYNAMIC.md`](docs/recon/08_VFS_DYNAMIC.md) — five-pass VFS order, precedence, and texture-quality mapping, measured in-process
- [`docs/recon/09_SCRIPTS.md`](docs/recon/09_SCRIPTS.md) — 8,666-script NCS corpus, validation, action usage, and decompile coverage
- [`docs/recon/10_SAVE_VFS.md`](docs/recon/10_SAVE_VFS.md) — save containers, archive I/O, and the five-pass VFS resolver
- [`docs/recon/11_GEOMETRY_PREVIEW.md`](docs/recon/11_GEOMETRY_PREVIEW.md) — m01aa LYT/WOK/MDL/PTH/GIT top-down preview and checks
- [`docs/adr/0001-runtime-and-renderer.md`](docs/adr/0001-runtime-and-renderer.md)
- [`docs/adr/0002-coordinates-and-render-features.md`](docs/adr/0002-coordinates-and-render-features.md)
- [`docs/adr/0003-distribution-and-install-model.md`](docs/adr/0003-distribution-and-install-model.md)
- [`tools/README.md`](tools/README.md) — tool usage and toolchain

## Legal and scope

This is an unofficial, non-commercial interoperability/research project. It is not affiliated with
or endorsed by BioWare, EA, Lucasfilm, Disney, or any platform holder. No game assets, extracted
data, save files, or decompiled program text are committed here.

You must supply your own legally acquired game installation. The project does not implement DRM,
license checks, or ownership verification. External GPL/LGPL tools are used as independent oracles
or authoring tools only; their code is not linked into the engine. A project license has not been
selected yet; no license is granted until one is added. Nothing in this repository is legal advice.

## Contributing

Evidence first: claims about the binary or formats must cite a build hash, address/offset, and
tool/test evidence. Keep game-derived artifacts out of commits, and keep implementation code
independent of GPL prior-art source.
