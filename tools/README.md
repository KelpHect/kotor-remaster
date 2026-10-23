# Tools

Local, read-only-against-the-game tooling for the KotOR interoperability program.
Nothing here redistributes game assets or decompiled code. Generated data goes under
`artifacts/` and is gitignored.

## Built here

| Tool | Purpose | Usage |
|---|---|---|
| `inventory/kotor_inventory.py` | KEY/BIFF/RIM inventory, per-BIF reconciliation, manifest hashes | see below |
| `inventory/resource_probe.py` | Reads payload magic per resource type; produces the verified type table | `python resource_probe.py --game-dir <install>` |
| `inventory/pe_summary.py` | PE32 headers, sections, imports, hashes for provenance | `python pe_summary.py <exe>` |
| `inventory/kotor_types.py` | Observed resource type table with confidence and evidence | imported by the inventory tool |
| `ghidra/ExportFunctionInventory.java` | Exports every Ghidra function to JSONL (optionally pseudocode) | see Ghidra section |
| `analysis/extract_actions.py` | Extracts `nwscript.nss`, parses hex + decimal table writes, writes the complete 772/772 action map and dispatcher metadata | `python tools/analysis/extract_actions.py --game-dir <install>` |
| `formats/tlk.py` | TLK V3.0 reader/validator (49,265 entries verified) | `python tools/formats/tlk.py <dialog.tlk> --check` |
| `formats/gff.py` | Read-only GFF V3.2 probe: structs/fields/lists, single-field struct rule, structural checks; validates IFO/GIT/ARE | `python tools/formats/gff.py <file.git> --check --root --entries` |
| `formats/two_da.py` | 2DA V2.b/V2.0 parser; validates all 209 shipped tables | `python tools/formats/validate_two_da.py --game-dir <install>` |
| `formats/test_two_da.py` | 29 synthetic 2DA parser tests | `python tools/formats/test_two_da.py` |
| `evidence/ledger.py` | SQLite artifact/function/action/resource/claim ledger and coverage view | `python tools/evidence/ledger.py init` then `status` |

```bash
python tools/inventory/kotor_inventory.py \
  --game-dir "<KOTOR_INSTALL>" \
  --out artifacts/inventory.json
```

## Ghidra

- Installed: Ghidra 12.1.4 at `<GHIDRA_INSTALL>` (Ghidra 12.1.4 PUBLIC used in this program).
- **Pinned JDK 21:** Temurin `21.0.12.1+1` at `tools/jdk21/jdk-21.0.12.1+1`
  (`java.exe` verified; gitignored). Archive SHA-256
  `f9d6e191ab098c0d416e7d588a24420a8621cd2f4720dab2459b8b7b2d2d8b4e`. Use it for reproducible
  headless runs; the initial import used JDK 25.
- Fresh project (artifact of record): `artifacts/ghidra/kotor.gpr`.
- The older Ghidra program with a `.bind` section and SHA-256 `34e6d971…` is a different build and
  is discarded.

Export after analysis:

```bash
cd "<GHIDRA_INSTALL>/support"
./analyzeHeadless.bat "<PROJECT_ROOT>/artifacts/ghidra" kotor \
  -process swkotor.exe -noanalysis \
  -scriptPath "<PROJECT_ROOT>/tools/ghidra" \
  -postScript ExportFunctionInventory.java \
  "<PROJECT_ROOT>/artifacts/ghidra/functions.jsonl"

# add --decompile as the second script argument for pseudocode + hashes
```

### Action coverage pass

`DefineActionHandlers.java` defines the dispatcher and the 440 `LAB_` action-handler targets on a
**copy** of the project, decompiles them, and writes `coverage.json`, `audit.tsv`, and
`new_functions.jsonl`. `VerifyActionHandlers.java` reopens the copy and re-checks entries, names,
comments, bodies, terminal instructions, and sample pseudocode. `ProbeActionHandlers.java` is the
read-only audit helper.

Result: 440 handlers + dispatcher created and decompiled (441/441, 0 failures). The copy has 9,057
internal functions (8,616 baseline + 441) plus 350 external imports (9,407 total function-manager
count). The original project and baseline export remain untouched.

A second pass (`RenameActionHandlers.java` / `VerifyRenamedHandlers.java`, also in this directory)
renamed the remaining 125 pre-existing `FUN_` handler targets to `ACT_<id>_<name>` and attached
shared-action comments. Validation: 565/565 targets named once, 772/772 actions mapped, dispatcher
present, 8,616 baseline functions retained, and 125/125 re-decompiled with matching hashes.

Java scripts are compiled into a shared cache; use one JDK per cache (a class compiled by JDK 25
cannot load under JDK 21). The pinned analysis runtime is JDK 21, but an existing JDK 25-compiled
script must be recompiled or run under JDK 25.

Upgrade target: Ghidra 12.2 once verified (CVE-2026-100503 affects ≤12.1.4).

## Planned external toolchain (licenses matter)

| Tool | Use | License / rule |
|---|---|---|
| [Kaitai Struct](https://kaitai.io/) + [OpenKOTOR/bioware-kaitai-formats](https://github.com/OpenKOTOR/bioware-kaitai-formats) | Format specifications and generated parsers | MIT; attribution required |
| [PyKotor](https://github.com/OpenKOTOR/PyKotor) | Test oracle / CLI extraction | LGPL-3.0+; do not copy into the engine |
| [xoreos-tools](https://github.com/xoreos/xoreos-tools) | Independent parser/NCS oracle | GPL-3.0; external process only |
| [kotorblender](https://github.com/seedhartha/kotorblender) | MDL/MDX/LYT/PTH asset authoring | GPL-3.0; external tool |
| DeNCS | NCS decompiler (best current coverage) | MIT; batch CLI; oracle only |
| xoreos-tools `ncsdis` / PyKotor | NCS disassembly, VM validation, compile | GPL-3.0 / LGPL-3.0+; external tools |
| FFmpeg | Bink 1 decode for movies | LGPL/GPL build; dynamic external use |
| SDL3 | Platform, window, input, audio | zlib |
| Vulkan SDK + VMA + glslang/SPIRV-Cross | Primary RHI and shader toolchain | Apache-2.0 / MIT |
| Dear ImGui | Debug/inspection UI | MIT |
| SQLite | Evidence/coverage store | Public domain |
| Frida, MinHook, x32dbg, apitrace, Mesa | Dynamic analysis, hooks, GL capture | LGPL / BSD / MIT; analysis only. RenderDoc is not usable for this legacy GL path; use a 32-bit apitrace build and Mesa llvmpipe as reference |
| CMake, Ninja, Python, vcpkg | Build/tooling | permissive |

Rules:
- Prefer MIT/Apache components in shipping code; GPL/LGPL tools run as external binaries for
  validation only and are never linked into the engine.
- No game assets, dialogue text, extracted meshes, or decompiled pseudocode are committed.
- Every external tool version is pinned in `docs/TOOLCHAIN.md` with source URL and hash.
