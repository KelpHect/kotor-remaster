# Legal and IP review (project-owned code and tooling)

Status: **review documented 2026-10-24.** This is an engineering review for the repository's
distribution model, not legal advice. License selection and public-release clearance remain
owner/release decisions (P6), not P0 evidence gates.

## What the repository contains

- Project-owned code: C++23 engine scaffold, Python parsers/tools/tests, documentation, and the
  windowed-patch tool. No BioWare/EA/Lucasfilm source code, assets, saves, extracted game data, or
  decompiled program text are committed.
- Third-party format specifications used as references (for example MIT-licensed Kaitai
  specifications) remain attributed to their authors; no GPL/LGPL implementation code is linked
  into the engine.
- `artifacts/` is local-only (gitignored): Ghidra projects, exports, downloaded saves, captured
  evidence, and test profiles. It is not part of the public repository.

## What the runtime does

- The runtime consumes a **user-supplied, legally acquired** KotOR 1.0.3.0 installation in place.
  It does not ship, download, extract, or redistribute game assets.
- The original `swkotor.exe` is an analysis oracle and is not required by the rebuilt runtime.
  The windowed-patch tool modifies only a user's own copy of that executable, keeps a backup, and
  ships no original bytes.
- No DRM, license-check, entitlement, or ownership-verification logic is implemented or bypassed.

## Clean-room separation

- The specification lane reads decompiler/runtime output and writes behavioral specifications,
  address tables, and fixtures in project-owned words; decompiler text stays in the local
  quarantined evidence store.
- The implementation lane writes engine code from specifications and tests only; no copied
  pseudocode, identifiers, comments, or structure, and no GPL code.
- Ghidra, Frida, and external format tools are independent oracles/authoring tools. Their licenses
  do not attach to project code because they are not linked into the shipped engine.

## Trademarks and presentation

- The project is unofficial and non-commercial; it is not affiliated with or endorsed by BioWare,
  EA, Lucasfilm, Disney, or any platform holder.
- The README carries the trademark disclaimer. Any future UI must not imply endorsement.
- Asset paths and names are read from the user's installation at runtime; no assets are packaged.

## Open release decisions (not P0)

1. Select a license for project-owned code (a permissive license such as MIT or Apache-2.0 is
   compatible with the clean-room model; Apache-2.0 adds an explicit patent grant).
2. Add a `NOTICE`/attribution file for any copied MIT-licensed format specifications and for
   third-party tools referenced by documentation.
3. Obtain a jurisdiction-specific legal review before any public release or binary distribution.
4. Maintain contributor provenance and the taint log for implementation-lane commits.
5. Review the user-supplied-install distribution model and EULA interactions for each target
   platform/store.

## Repository checks performed

- `git ls-files` contains no `swkotor.exe`, save folders, `dialog.tlk`, BIF/RIM/ERF payloads,
  extracted MDL/MDX/TPC/TGA data, or captured desktop screenshots.
- The windowed-patch tool contains only our trampoline bytes and verification hashes; it refuses
  unexpected executables and writes backups outside the install.
- Dynamic evidence remains under `artifacts/`; the lane summaries cite hashes and game-window-only
  captures.
