# P0 foundation report — strict closure in progress

Status: **reopened for strict evidence closure; not 100% complete.** The earlier foundation-gate
acceptance did not establish full decompilation, runtime equivalence, or save acceptance. Under the
requested stricter standard, unresolved or blocked observations remain incomplete; they are not
waivers. Analyzer confidence and byte accounting alone do not prove behavioral completeness.

## Historical foundation checks (not strict completion)

| # | P0 item | Result | Evidence |
|---:|---|---|---|
| 1 | Freeze manifest; pin JDK 21; validate fresh Ghidra project hash | **met** — 15,380 files / 3,602,889,094 B; JDK 21.0.12.1+1; target SHA-256 `0290889e…` | `docs/recon/06`, `tools/README.md` |
| 2 | Export all functions; classify externals/thunks/CRT/middleware vs engine | **met** — exhaustive 11,199 rows: 10,302 engine, 441 CRT, 350 external, 76 thunks, 30 helpers, 0 unknown; 0 unexplained `.text` bytes | `docs/recon/14` |
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
* **Function inventory:** v4 exhaustive inventory 10,849 internal + 350 external, all high
  confidence; 0 unexplained `.text` bytes; baseline 8,616 pseudocode export retained, v4
  export/ledger consolidation queued.
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

## Outstanding strict-closure work

* Consolidated v4 pseudocode/body-hash export and ledger reconciliation are pending. The reported
  10,849 internal functions and assigned `.text` ranges are an analysis inventory, not proof that
  every original function or behavior has been recovered.
* Downloaded manual/autosave files now exist in the lane profile (provenance in
  `artifacts/lanes/06-dynamic/cont3/SAVE_PROVENANCE.md`). Retail loading, module activation, and
  save-write/reload acceptance remain unverified; structural parser acceptance is insufficient.
* The resumed clean-room NCS checkpoint contains 8,666 rows: 7,003 exact instruction-stream
  matches, 514 reachable-only matches, 846 mismatches, and 303 compile failures. The reachable-only
  comparison still needs deferred-action/STORESTATE coverage verification. Do not combine it with
  exact matches as proof of full equivalence. The separate lossless bytecode codec is not source
  decompilation.
* Engine-side CameraAngle framing, supermodel override/node remap, and skin qbone/tbone/aux4
  consumers remain under investigation. Full MDX byte accounting does not close these semantics;
  multi-group behavior and bbox/rendering exceptions retain their limits.
* Dynamic probes remain windowed. Without explicit foreground consent, they require verified
  isolated virtual-screen execution; parking at `-32000` is not isolation. With the user's explicit
  approval for a focused main-screen test, game-window activation/focus and normal UI input are
  allowed and no virtual screen is required. No repeated focus reclamation, full-desktop capture,
  auto-overwriting install guards, or process-name-wide manipulation. See `docs/SPRINT0.md`.
* No engine renderer yet; the scaffold clears and draws ImGui only.
* Public license and IP review remain open; the external decompiler remains an analysis oracle,
  not a shipping dependency.

## Reproduce

```bash
export KOTOR_GAME_DIR="<KOTOR_INSTALL>"
python tools/install/install_validate.py
python tools/install/cache_profile.py build --out artifacts/cache/profile.json
python tools/install/cache_profile.py verify --profile artifacts/cache/profile.json --game-dir "$KOTOR_GAME_DIR"
KOTOR_GAME_DIR="$KOTOR_GAME_DIR" python tools/tests/run_all.py
```

P1 work can reuse the foundation measurements, but it does not close the outstanding strict-P0
items above. Completion requires reviewed evidence for each item, not only a passing gate summary.
