# P0 exit report — Foundation

Status: **complete with documented residual unknowns.** Every P0 work item and the Gate G0
requirements are satisfied. "Complete" does not mean every binary function is semantically
specified or that a real save exists; those residuals are listed below and do not block P1.

## P0 checklist

| # | P0 item | Result | Evidence |
|---:|---|---|---|
| 1 | Freeze manifest; pin JDK 21; validate fresh Ghidra project hash | **met** — 15,380 files / 3,602,889,094 B; JDK 21.0.12.1+1; target SHA-256 `0290889e…` | `docs/recon/06`, `tools/README.md` |
| 2 | Export all functions; classify externals/thunks/CRT/middleware vs engine | **met** — 9,407 items labeled: 8,527 engine, 417 CRT, 350 external, 61 thunks, 30 compiler helpers, 22 unknown | `docs/recon/14` |
| 3 | Evidence DB + repo skeleton | **met** — SQLite ledger (27 cited claims), public repo, lane artifacts | `tools/evidence/ledger.py`; repo |
| 4 | Install detection/validation + external cache profile | **met** — validator, deterministic profile (`6bf42d3c…`), 14 tests, output-refusal guard | `docs/recon/13` |
| 5 | Original run under file/registry monitor; baseline settings/saves | **met with save caveat** — 332 read-only install opens/run, only profile `swkotor.ini` written, registry exports byte-identical; no save exists to capture | `docs/recon/08`; lane 06 `cont/` |
| 6 | Gate G0: reproducible tools, hash match, inventory reconciled, clean-install validate/cache | **met** — profile byte-identical across runs; manifest 0 missing/extra/size diffs; validator + verify pass without writes | `docs/recon/13`; `artifacts/lanes/p0-install-cache/out/` |

## Gate G0 detail

* **Reproducible tool output:** two cache-profile builds were byte-identical
  (`ca86e259…5bbbafe`); profile hash `6bf42d3c…348fd1`; install snapshot before/after identical.
* **Analysis hash match:** Ghidra target SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`;
  baseline project never modified by classification.
* **Inventory reconciled:** 15,380 files / 3,602,889,094 B, 0 missing, 0 extra, 0 size diffs;
  dataset subset 279 files / 2,113,227,942 B, 0 hash/size disagreements.
* **Function inventory:** 8,616 baseline functions fully decompiled; 9,057 internal + 350 external
  in the coverage/classification set; digests recorded.
* **Clean install:** `install_validate.py` returns VALID with 0 errors/warnings; `cache_profile.py
  verify` matches 279/279; both refuse to write inside the install.

## Dynamic baseline (P0 item 5)

Three off-screen runs reached `MAINMENU.rim`:

* 436 relevant file opens per run; 332 resolve into the install and are all `GENERIC_READ` /
  `OPEN_EXISTING`.
* The only write-intent opens are 14 `.\swkotor.ini` opens in the profile; the profile ini is
  rewritten with identical content. The install is unchanged.
* Registry exports before/after are byte-identical (`34f1f526…`). The only KotOR key is
  `HKLM\SOFTWARE\WOW6432Node\BioWare\SW\KotOR` (`path`, `flavour=Retail`); no HKCU/LucasArts key.
  18 write attempts per run were blocked (AMD + DirectInput), none KotOR-related.
* No `saves/` or `modules/*` resource is opened; the reference install has no save folder.
  Capturing a save would require driving character creation/module load/save UI, which this harness
  does not do. This is a **blocked observation**, not a missing implementation.

## Residual unknowns (carried into P1)

* 22 disconnected functions remain `unknown`; 3,464 engine rows are medium-confidence,
  region/call-graph based. Classification is a triage mask, not semantic specification.
* No real save exists; the save writer is structurally validated but not byte-compatible or
  retail-save-tested.
* Source-level NCS decompilation is 60.72% reproducible; the bytecode codec itself is lossless
  (8,666/8,666) and is the recommended verification path.
* Animation controller payloads, MDX trailing LOD/skin data, and multi-group descriptors remain open.
* No engine renderer yet; the scaffold clears and draws ImGui only.
* Public license and IP review remain open; DeNCS/NCSDecomp is BSL 1.1 and is an oracle only.

## Reproduce

```bash
export KOTOR_GAME_DIR="<KOTOR_INSTALL>"
python tools/install/install_validate.py
python tools/install/cache_profile.py build --out artifacts/cache/profile.json
python tools/install/cache_profile.py verify --profile artifacts/cache/profile.json --game-dir "$KOTOR_GAME_DIR"
KOTOR_GAME_DIR="$KOTOR_GAME_DIR" python tools/tests/run_all.py
```

P1 starts from this frozen foundation: data completeness, round-trip suites, the coordinate
transform contract, and the per-install asset cache.
