# Legal and IP review (project-owned code and tooling)

Status: **review documented 2026-10-24; repository audit and release checklist added
2026-11-21.** This is an engineering review for the repository's distribution model, not legal
advice. License selection and public-release clearance remain owner/release decisions (P6), not
P0 evidence gates.

Companion documents: [`LICENSE_PROPOSAL.md`](LICENSE_PROPOSAL.md) (license options for the
owner; nothing is selected) and [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)
(dependency and reference attribution).

## What the repository contains

- Project-owned code: C++23 engine scaffold, Python parsers/tools/tests, documentation, and the
  windowed-patch tool. No BioWare/EA/Lucasfilm source code, original asset files, or decompiled
  program text are committed. The 2026-11-21 audit found committed evidence that is derived from
  the game (engine-written save folders, screenshots, dialogue lines in logs); see
  "Audit findings" below.
- Third-party format specifications used as references (for example MIT-licensed Kaitai
  specifications) remain attributed to their authors; no `.ksy` file or generated parser is
  committed. No GPL/LGPL code is vendored or fetched by the default build. The optional Bink
  path links an external FFmpeg build (finding F3).
- `artifacts/` is local-only (gitignored): Ghidra projects, exports, downloaded saves, captured
  evidence, and test profiles. It is not part of the public repository.

## What the runtime does

- The runtime consumes a **user-supplied, legally acquired** KotOR 1.0.3.0 installation in place.
  It does not ship, download, extract, or redistribute game assets.
- The original `swkotor.exe` is an analysis oracle and is not required by the rebuilt runtime.
  The windowed-patch tool (`tools/mod/kotor_windowed_1280.py`, a developer analysis tool, not
  part of the runtime) modifies only a user's own copy of that executable, keeps a backup beside
  it, and ships no copy of the executable. It does embed a few short original byte sequences
  (5-byte patch-site checks and the two relocated instructions re-emitted in its trampoline).
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

## Audit findings (2026-11-21)

Static audit of `master` from a machine with no game install and no GPU. Each finding names what
was observed; none is a conclusion that content infringes.

| # | Finding | Evidence | Release effect |
|---|---|---|---|
| F1 | Save folders are committed. | `docs/evidence/s9/saves/000009 - ENGINE/`, `.../000010 - ENGINE/`, `docs/evidence/s11/000012 - ENGINE/` (15 files: `SAVEGAME.sav`, `GLOBALVARS.res`, `PARTYTABLE.res`, `savenfo.res`, `Screen.tga`). They are written by this engine, but each `SAVEGAME.sav` embeds a module-state archive (`end_m01aa`, `tar_m02af`) built from the user's game data, and `Screen.tga` is a rendered frame. | Contradicts the README and the earlier text of this review. Blocker R1. |
| F2 | Rendered game imagery and dialogue text are committed. | 42 PNG files under `docs/evidence/s5`..`s14` showing game art, rendered by the engine except `s13/bik-oracle.png`, which is an FFmpeg decode of a frame of the game's `01a.bik`; `s13/movie-*.png` are engine captures of that movie; 7 evidence logs quote game dialogue lines. `docs/evidence` is 63 MB. | Screenshots and short quotes are common in fan projects but are game-derived content; the "no assets" wording must match what is shipped. Blocker R1. |
| F3 | The optional Bink path links FFmpeg, and the documented build is a GPL one. | `docs/versions.md` pins BtbN `win64 gpl-shared`; `engine/CMakeLists.txt` links it into `kotor_data` and copies the DLLs beside `kotor_engine`. Configure builds without it when no FFmpeg is found at `KOTOR_FFMPEG_ROOT`. | A binary distributed with those DLLs must be offered under GPL terms. Interacts with the license choice; blocker R3. |
| F4 | The windowed-patch tool writes inside the install. | `kotor_windowed_1280.py` writes `swkotor.exe.orig-1.0.3.0` beside the executable and replaces `swkotor.exe`; it checks the SHA-256 first and supports `--revert`. | Earlier text said backups go outside the install; corrected above. Keep the tool out of release packages (R6). |
| F5 | Single-author history; the spec and implementation lanes cannot be told apart from git alone. | All commits are by one author; 7 engine sources cite original-binary addresses (`FUN_00...`, data addresses) in comments; Ghidra scripts live in `tools/ghidra/`. No taint log file is committed. | The repo documents a spec/implementation split, but a strict two-team clean room cannot be shown from the public record. Counsel should see this (R4, R5). |
| F6 | No `LICENSE`, `NOTICE`, or contribution-terms file. | Repository root. README states no license is granted. | Expected until the owner decides; R2. |
| F7 | `tools/compiler/nwnnsscomp-historical.patch` carries upstream context lines. | Patch against `virusman/nwnexplorer` `ead0f176...`; upstream sources carry a BSD-style notice (Copyright (c) 2002-2003 Edward T. Smith). | Attribution recorded in `THIRD_PARTY_NOTICES.md`; the built compiler is never shipped. |
| F8 | Runtime writes default to a per-user directory. | `SDL_GetPrefPath("kotor-remaster", "engine")` holds `settings.ini` and `saves/`; `--saves-dir`, `--settings-file` and report/capture options take caller-supplied paths. The static search found no write path built from the install root. | Static only; runtime confirmation is W2. |
| F9 | Test fixtures are project-authored or ID-only. | `engine/tests/fixtures/*.nss` are synthetic scripts with their compiled `.ncs`; baselines state "tags, resrefs and numeric IDs only"; `vfs/*.2da` are synthetic merge inputs. | No action; keep the rule when adding fixtures. |
| F10 | SPIR-V binaries are compiled from the project's own GLSL. | `engine/shaders/*.spv` beside `.vert`/`.frag`. | No action. |

## Release blockers

A public source release needs R1, R2, R4 and R5. A binary release needs all of them.

- **R1 Evidence content.** Decide per class: remove from the tree (and from history if the
  history is published), or keep and reword the README and this review to say that
  engine-rendered screenshots and engine-written saves are included as evidence. Suggested
  default: move the three save folders and the Bink movie frames to `artifacts/`, keep
  UI/parity screenshots, and replace quoted dialogue in logs with StrRefs.
- **R2 License.** Owner picks from `LICENSE_PROPOSAL.md`; then add `LICENSE`, update the README
  badge and "Legal and scope" text, and add contribution terms (DCO sign-off or equivalent).
- **R3 FFmpeg.** Ship binaries without FFmpeg, or switch to an LGPL shared build (confirm the
  Bink decoder works in it) and meet the LGPL notice/relink terms, or accept GPL terms for the
  binary. Record the choice in `docs/versions.md`.
- **R4 Provenance.** Commit the taint log and a contributor provenance statement; decide whether
  original-binary addresses stay in engine comments.
- **R5 Counsel.** Jurisdiction-specific review of the reverse-engineering basis, F1/F2/F5, the
  trademark use in the project name and README, and the EULA of each store version.
- **R6 Package contents.** A release package holds the engine, its shaders, the notices file and
  dependency license texts, plus the FFmpeg DLLs and their license text when R3 keeps FFmpeg:
  no `docs/evidence`, `tools/mod`, `tools/ghidra` or harness hooks.
- **R7 Notices in binaries.** Ship `THIRD_PARTY_NOTICES.md` and the license texts of SDL3,
  Vulkan-Headers, volk, Dear ImGui and dr_mp3 (and FFmpeg if included) with every binary.

## Checks left for the owner's Windows PC

These need the KotOR 1.0.3.0 install and a GPU and were not run.

- **W1 Install hash baseline.** Hash every file under the install (Steam and GOG if both are
  owned) before a session.
- **W2 Read-only install.** Run a full session (new game, save, load, movie, options change,
  quit) with the install directory read-only or under Process Monitor; then re-hash and compare
  with W1. Expected: zero writes, zero new files. Repeat with `--saves-dir` and the
  capture/report options.
- **W3 User-data location.** Confirm `settings.ini`, `saves/` and any cache appear only under the
  `SDL_GetPrefPath` directory (per-user application data, `kotor-remaster/engine/`) and that
  removing it resets the engine without touching the install.
- **W4 No original executable.** Rename `swkotor.exe` and the store launcher files and confirm
  the engine still starts and plays; confirm no network activity during a session.
- **W5 Windowed-patch tool.** `--check`, `--apply`, `--revert` on a copy of the executable:
  refusal on a wrong hash, backup created, revert restores the W1 hash.
- **W6 Clean build output.** From a fresh clone, list the build output and confirm it holds no
  file from the install and no file from `docs/evidence`.
- **W7 FFmpeg variants.** Build and play a movie with no FFmpeg, with the GPL build, and with an
  LGPL shared build; record which DLLs are copied and whether Bink decodes in the LGPL build.
  Use a fresh build directory for each variant: `KOTOR_FFMPEG_ROOT` and the found library paths
  are cached by CMake, so changing the environment variable alone keeps the previous build.
- **W8 Evidence provenance.** For F1/F2, confirm from the local `artifacts/` store that every
  committed save and image was produced by this engine and not by the original executable, and
  that no committed image is a desktop capture.
- **W9 Presentation.** Check window title, menus, credits and crash dialogs for the
  unofficial/non-affiliated wording and for no original logos beyond what the user's own data
  renders.
- **W10 Store terms.** Read the Steam and GOG KotOR EULA shown by the owner's installs and note
  any clause on reverse engineering or third-party executables for R5.

## Open release decisions (not P0)

1. Select a license for project-owned code. Options and consequences are in
   [`LICENSE_PROPOSAL.md`](LICENSE_PROPOSAL.md); the owner decides (R2).
2. Attribution is recorded in [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md); keep it
   current when a dependency or reference changes.
3. Obtain a jurisdiction-specific legal review before any public release or binary distribution.
4. Maintain contributor provenance and the taint log for implementation-lane commits.
5. Review the user-supplied-install distribution model and EULA interactions for each target
   platform/store.

## Repository checks performed

- `git ls-files` contains no `swkotor.exe`, `dialog.tlk`, BIF/RIM/KEY payloads, or extracted
  MDL/MDX/TPC/WAV/BIK data (re-checked 2026-11-21). It does contain three engine-written save
  folders and engine-rendered screenshots (F1, F2).
- The windowed-patch tool contains our trampoline bytes, verification hashes and the short
  original byte sequences noted above; it refuses
  unexpected executables and writes its backup beside the executable (F4).
- No `.ksy` specification, generated Kaitai parser, or GPL/LGPL source file is committed; the
  default CMake configure fetches only SDL3, Vulkan-Headers, volk and Dear ImGui by pinned hash.
- Dynamic evidence remains under `artifacts/`; the lane summaries cite hashes and game-window-only
  captures.
