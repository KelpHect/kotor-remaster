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
- Script action table: **772/772 actions mapped**; 440 handler functions + dispatcher added and
  decompiled in an isolated project copy (9,057 internal functions; 350 external imports).
- Archive inventory: 25,836 base BIF resources, 20,575 module RIM resources, 1,936 global-RIM
  resources, plus patch/texture-pack ERFs; 49,265 TLK strings.
- TLK V3.0 parser validated; SQLite evidence ledger; action-table extraction tooling.
- Architecture decisions for the runtime/renderer, coordinate + map-compilation contract, and the
  user-supplied-install distribution model.

## Repository layout

```
docs/        master plan, reconnaissance, ADRs, format/VM notes
tools/       read-only inventory/parsers, Ghidra exporters, action-table and evidence tooling
artifacts/   local only (gitignored): Ghidra projects, exports, caches, captured evidence
```

Start with:

- [`docs/MASTER_PLAN.md`](docs/MASTER_PLAN.md) — program lanes, phases, gates, verification
- [`docs/recon/01_ARTIFACTS.md`](docs/recon/01_ARTIFACTS.md) — verified artifact/content inventory
- [`docs/recon/02_TECHNICAL_NOTES.md`](docs/recon/02_TECHNICAL_NOTES.md) — format, VM, VFS, rules notes
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
