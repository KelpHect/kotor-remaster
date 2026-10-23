# Sprint 0 exit report

Status: **complete with documented gaps.** Ten lanes ran against a user-owned KotOR 1.0.3.0
install; all outputs reviewed, testable results integrated, and the public repository contains no
game assets, extracted data, or decompiled program text. Three lanes (04, 06, 10) hit their agent
turn limit; their datasets were complete enough for the integrator to finish the reports.

## Exit criteria

| # | Criterion | Result | Evidence |
|---:|---|---|---|
| 1 | Install manifest | **met** — 15,380 files, 3,602,889,094 B, SHA-256 manifest, install unchanged | `docs/recon/06_MANIFEST.md` |
| 2 | Parsers | **met** — GFF 11,143/11,143; 2DA 209/209; TLK 49,265; ERF/RIM/BIFF/KEY readers + tests | `docs/recon/05`, `07`, `10`; `tools/` |
| 3 | Scripts | **met with coverage gap** — 8,666 unique NCS all decode; 285,015 ACTION ops, 0 unknown IDs; 5,262 reproducible | `docs/recon/09_SCRIPTS.md` |
| 4 | Save/VFS | **met except real save** — 33 tests, 32 pass/1 skip; dynamic five-pass + quality mapping | `docs/recon/08`, `10` |
| 5 | Dynamic harness | **met** — one local process session (Frida), VFS/VM/RNG/combat hooks; off-screen, no install writes | `docs/recon/08`; lane 06 logs |
| 6 | Ghidra | **met** — 8,616 baseline, 9,057 coverage functions; 565/565 targets, 772/772 actions, 125 renamed | `docs/recon/02`; lane 07 |
| 7 | Engine scaffold | **met** — builds `/W4 /WX`, CTest 2/2, Vulkan 1.4 and GL 4.5 120-frame clears | `engine/docs/BUILD.md` |
| 8 | Geometry path | **met** — m01aa LYT/WOK/MDL/PTH/GIT preview, 45/45 checks | `docs/recon/11_GEOMETRY_PREVIEW.md` |
| 9 | Evidence/repo | **met** — ledger 24 cited claims; lane outputs integrated; no assets/secrets | `docs/recon/`; public repo |

## Lane results

| Lane | Outcome |
|---|---|
| 01-manifest | 15,380 files / 3.36 GiB hashed; no symlink escape |
| 02-2da | 209/209 tables, 29 tests; verified delimiter/address semantics |
| 03-gff | 11,143 payloads, 1.85M values, 0 intrinsic failures; corrected slot invariant |
| 04-ncs | 8,666 unique scripts; 4.70M instructions; action usage; 60.72% round-trip |
| 05-save-vfs | Save parser/writer, archive I/O, five-pass resolver; 33 tests |
| 06-dynamic | Five-pass order, first-hit precedence, Override shadow, quality mapping |
| 07-ghidra | 125 `FUN_` handlers renamed; 565/565 targets and 772/772 actions; baseline retained |
| 08-engine | C++23/SDL3/Vulkan 1.4/GL 4.5/ImGui scaffold, promoted to `engine/` |
| 09-geometry | m01aa preview and coordinate checks (one 51 mm door outlier) |
| 10-coords | 699 doors, 312 use points, 1,099 cameras, 106 areas; Bearing/camera/NorthAxis |

## Key verified facts

* **GFF**: single-field structs are direct field rows; multi-field slot ranges are disjoint but not
  ordered by struct index. Locstrings use `total = 8 + substring bytes`.
* **Scripts**: all ACTION instruction ids are known; 466 of 772 actions are used; five scripts carry
  arity anomalies that must be reproduced, not “fixed”.
* **VFS**: directories → ERF tag 1 → RIMs → ERF tag 2 → KEY/BIF, first hit wins; Override beats a
  texture pack; `Texture Quality` 2/1/0 selects tpa/tpb/tpc, with gui always loaded.
* **Coordinates**: 1 unit = 1 m, RH Z-up; IFO entry is a ground point; WOK vertices are world-space;
  LYT origin = MDL model origin; WXYZ quaternions; Bearing is radian yaw with RH `R_z(+B)` verified
  under the use-point invariant; doors select models by UTD `GenericType`; camera position is a
  ground point with `eye = z + Height`; ARE `NorthAxis` is absent; `WOK.position` is unused for
  room placement.
* **Ghidra**: dispatcher at `0x0052c0d0`; `0x0054c960` allocates the 772-slot table; service vtable
  `0x7450e4`; `-2002` null/out-of-range result.

## Known gaps carried into P1

1. **MDX vertex/face blocks** and animation controller payloads/supermodel override mechanics are
   undecoded (the header survey and name-inheritance rule are bounded evidence).
2. **Door appearance/facing and `WOK.position` placement are resolved.** Open coordinate items:
   room-WOK authoring-frame semantics, the minimap transform, and DLG/GIT camera transforms.
3. **No real save** exists in the reference install; the save writer is structural, not
   byte-compatible or retail-verified.
4. **NCS decompiler coverage**: 3,404/8,666 scripts do not reproduce; a direct NCS→IR path or a
   better decompiler is needed.
5. **Real intra-pass order** (two same-tier archives with a duplicate resource) is unmeasured.
6. **Engine**: clear + ImGui only; no depth/mesh pipeline, no asset rendering, no Vulkan validation
   layer installed; swapchain format changes are fatal.
7. **Legal**: license TBD; external GPL/LGPL tools are oracles only; public release needs IP review.

## Reproduce the foundation

```bash
# install root, never modified
export KOTOR_GAME_DIR="<KOTOR_INSTALL>"

python tools/inventory/kotor_inventory.py --game-dir "$KOTOR_GAME_DIR" --out artifacts/inventory.json
python tools/formats/validate_two_da.py --game-dir "$KOTOR_GAME_DIR"
python tools/tests/run_all.py
python -m unittest -v tools/formats/test_gff_types.py   # run from tools/formats

# engine scaffold (MSVC)
cd engine && scripts\configure.bat msvc-release && scripts\build.bat msvc-release
ctest --preset msvc-release --output-on-failure

# geometry preview (flat extracted m01aa directory)
KOTOR_AREA_DIR="<area-dir>" KOTOR_PREVIEW_OUT="<out-dir>" python tools/geometry/preview.py
```

## Public repository

`https://github.com/KelpHect/kotor-remaster` — master, initial commit `520c091`, foundation work
through `80902ba` and this report. No assets, saves, extracted payloads, or decompiled program text
are tracked; `artifacts/` remains local. The next phase is P1 data completeness and the
coordinate/transform contract freeze described in `docs/MASTER_PLAN.md`.
