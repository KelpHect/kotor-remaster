# P0 install validation and external cache

Status: **install detection, validation, and the external cache profile are implemented and tested.**
This closes the P0 install/cache item and supplies the clean-install part of Gate G0. Function
classification and the registry baseline are separate P0 items still in progress.

## Tools

* `tools/install/install_validate.py` — stdlib, read-only detection/validation. Detects the root
  from an explicit directory, a file, or a child directory using `chitin.key`, `dialog.tlk`,
  `patch.erf`, `data/*.bif`, `modules/`, and `rims/`. Parses KEY/BIFF structure and counts, TLK
  version/language/string count, RIM magics, and the `patch.erf` key table. Product version comes
  from a dataset fingerprint (`chitin.key` SHA-256), not from `swkotor.exe`, so the executable is
  not required or read.
* `tools/install/cache_profile.py` — builds/verifies an external JSON profile with dataset id,
  version, language, scope, per-file path/size/SHA-256/kind, counts, and `profile_hash`. Output is
  refused inside the install (or any ancestor containing `chitin.key`/`swkotor.exe`).
* `tools/tests/test_install_cache.py` — 14 tests: real install, synthetic valid/missing/wrong-hash
  cases, profile determinism/verification, same-size tamper, deletion, append, and output refusal.
  The real-install case skips when `KOTOR_GAME_DIR`/`KOTOR_INSTALL` is absent.
* `tools/harness/install_guard.py` — watches the manifest's config files during dynamic runs and
  restores exact bytes/mtime if the game rewrites them.

## Measured on the reference install

After restoring the config file (see incident below):

| check | result |
|---|---|
| validator | VALID, 0 errors, 0 warnings |
| KEY / BIFF | 25,836 resources, 26 BIFs, stride 22 |
| TLK | 49,265 strings, English (language id 0) |
| dataset | `kotor1-pc-1.0.3.0-english`, product 1.0.3.0 |
| modules / rims | 234 RIMs / 72,466,498 B; 12 RIMs / 39,034,549 B; 0 bad |
| patch.erf | 97 entries |
| cache profile | 279 files / 2,113,227,942 B; `profile_hash 6bf42d3c…348fd1` |
| profile verify | 279/279 match |
| manifest reconciliation | 15,380 files / 3,602,889,094 B; 0 missing, 0 extra, 0 size diffs |
| dataset subset vs manifest | 279 files / 2,113,227,942 B; 0 hash/size disagreements |

Two builds of the profile were byte-identical (`ca86e259…5bbbafe`). A before/after
`(path,size,mtime_ns)` snapshot of all 15,380 files was identical, proving validation and profile
verification do not write the install.

Format facts confirmed: KEY BIF-name offsets are absolute file offsets; TLK V3.0 entries are 40
bytes (`20 + count*40 == entriesOffset`).

## Incident: install `swkotor.ini` rewritten

The P0 reconciliation found the install's `swkotor.ini` at 2,648 B instead of the manifest's
2,607 B; the game had appended `RefreshRate=60`, `Height=1200`, and `Width=1600` during a dynamic
run even though the harness launched a profile copy. The exact original was reconstructed and
verified against the manifest: size 2,607 B, SHA-256
`85b1c89b4acb299e4de65633784fef56b22e64c93ea5064948c97bb3e8c059ef`, original mtime restored. The
modified 2,648 B copy is preserved at
`artifacts/lanes/06-dynamic/incident/swkotor.ini.modified-2648` (untracked).

Rule from now on: run `tools/harness/install_guard.py` for the lifetime of any dynamic session. It
verifies config files against the frozen manifest at startup and restores them if changed. The
install is back to a 0-delta manifest match.

## Clean-install workflow

```bash
export KOTOR_GAME_DIR="<KOTOR_INSTALL>"
python tools/install/install_validate.py
python tools/install/cache_profile.py build --out artifacts/cache/profile.json
python tools/install/cache_profile.py verify \
    --profile artifacts/cache/profile.json --game-dir "$KOTOR_GAME_DIR"
```

Use `profile_hash`/`dataset.id` as the cache key. The profile covers dataset files only (BIF, KEY,
TLK, module/global RIMs, patch ERF, texture packs); streamed media and launcher/config files are
stat-only. `--scope all` hashes every regular file when a manifest-equivalent check is needed.

## Limitations

* Version detection is a fingerprint lookup against the one reference dataset; an unrecognized
  mirror reports a generated id instead of guessing.
* Only the default data stream is hashed (no ADS/ACL/sparse-extent checks).
* A concurrent writer during hashing is not locked; re-verify on launch rather than trusting a
  stored total.
* The registry baseline and full function classification remain separate P0 items.
