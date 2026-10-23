# Sprint 0 — Foundation exit criteria

Status: in progress (ten lanes launched).
Goal: finish the evidence/tooling foundation and prove the data→geometry path. A full engine-rendered
area and gameplay parity belong to P4/P5, not Sprint 0.

## Exit criteria

1. **Artifact manifest** — every file in the user's install hashed (SHA-256) with sizes/kinds; evidence
   exports hashed; no game content committed.
2. **Parsers** — GFF (all shipped GFF resources), 2DA (all shipped tables), TLK (all 49,265 strings),
   and archive containers validated; failures triaged; reusable parsers/tests in the public toolset.
3. **Scripts** — every unique NCS extracted, hashed, and paired with NSS where shipped; per-file
   decompile/disassembly status published; action table fully named.
4. **Save/VFS** — save parser/writer for the documented layout; synthetic VFS precedence fixtures;
   real-save validation when a save exists; texture-pack quality selection measured.
5. **Dynamic harness** — one original-process session using Frida/native hooks, capturing file/VFS,
   VM/action, RNG, combat, and/or save evidence; blockers documented; install not modified.
6. **Ghidra** — all 125 remaining `FUN_` action-handler targets named/commented; 565 targets and 772
   actions mapped; final project snapshot and export preserved without touching the baseline.
7. **Engine scaffold** — C++23 + SDL3 + Vulkan 1.4 (with GL 4.5 fallback and ImGui) builds and opens a
   window/clears; build instructions and log recorded.
8. **Geometry path** — first area (m01aa/Endar Spire) loaded from original LYT/WOK/PTH/GIT/IFO with a
   rendered wireframe/top-down preview, coordinate fixtures passing.
9. **Evidence/repo** — ledger updated; lane outputs integrated; public repo contains no assets/secrets;
   Sprint 0 exit report written.

## Lanes (wave 1)

| Lane | Scope | Output root |
|---|---|---|
| 01-manifest | full install + evidence hashes | `artifacts/lanes/01-manifest/` |
| 02-2da | binary/ASCII 2DA parser + all tables | `artifacts/lanes/02-2da/` |
| 03-gff | all-resource GFF validation | `artifacts/lanes/03-gff/` |
| 04-ncs | NCS/NSS corpus + decompile status | `artifacts/lanes/04-ncs/` |
| 05-save-vfs | save parser/writer + VFS fixtures | `artifacts/lanes/05-save-vfs/` |
| 06-dynamic | Frida/game harness + VFS measurement | `artifacts/lanes/06-dynamic/` |
| 07-ghidra-rename | name remaining 125 handlers | `artifacts/lanes/07-ghidra-rename/` |
| 08-engine | C++23/SDL3/Vulkan 1.4/GL/ImGui scaffold | `artifacts/lanes/08-engine/` |
| 09-geometry-preview | m01aa LYT/WOK/PTH/GIT preview | `artifacts/lanes/09-geometry-preview/` |
| 10-coords-open | Bearing/camera/door/MDX/animation semantics | `artifacts/lanes/10-coords-open/` |

## Shared-resource rules

- **Game process:** lane 06 only. It must not patch/bypass DRM, modify the install, or leave files it
  created; all captures under its lane directory. Runs are windowed (`FullScreen=0`), off-screen, and
  at idle priority: use `tools/harness/offscreen.py` (private desktop or park/watch) and never take
  foreground focus or capture the user's desktop. Only the game window may be captured.
- **Ghidra:** lane 07 only, on a fresh copy of the enhanced project. No other lane opens a Ghidra
  project or runs analyzeHeadless.
- **Repository:** no lane writes to tracked files, commits, pushes, or changes the public repo. The
  integrator promotes reviewed outputs.
- **Game data:** never copied into tracked files; lane outputs stay under `artifacts/`.
- **Evidence:** every claim needs an address/offset/file hash/test, and verified/inferred/open status.

## Completion log

| Lane | Status | Evidence |
|---|---|---|
| 01-manifest | complete | 15,380 files / 3,602,889,094 B hashed; manifest SHA-256 recorded; install unchanged |
| 02-2da | complete | 209/209 tables, 29 tests; `tools/formats/two_da.py`; `docs/recon/05_2DA.md` |
| 03-gff | complete | 11,143/11,143 payloads, 1,854,982 values, 0 intrinsic failures; parser patch + 30 tests; `docs/recon/07_GFF_CORPUS.md` |
| 04-ncs | datasets complete (agent turn-limited) | 8,666 unique NCS, 8,666 valid, 285,015 ACTION ops, 0 unknown ids; 5,262/8,666 reproducible; `docs/recon/09_SCRIPTS.md` |
| 05-save-vfs | complete | 33 tests (32 pass, 1 skipped: no real save); save reader/writer, archive readers/writers, five-pass VFS resolver; `docs/recon/10_SAVE_VFS.md` |
| 06-dynamic | complete | five-pass order, first-hit precedence, Override shadow, and quality 2/1/0 → tpa/tpb/tpc measured in-process; `docs/recon/08_VFS_DYNAMIC.md` |
| 08-engine | complete | `engine/` promotes the scaffold; 313-step MSVC `/W4 /WX` build, CTest 2/2, Vulkan 120 frames, GL 120 frames; `engine/docs/BUILD.md` |
| 09-geometry-preview | complete | m01aa SVG preview, 1365/1365 winding, 45/45 cross-checks; `artifacts/lanes/09-geometry-preview/` |
| 07-ghidra-rename | complete | 125/125 renamed, 565/565 targets named, 772/772 actions, baseline retained; `artifacts/lanes/07-ghidra-rename/` |

## Out of scope for Sprint 0

Full gameplay parity, complete engine renderer/UI/audio, content-wide map compilation, and public
release legal clearance. These remain P4/P5/P6.
