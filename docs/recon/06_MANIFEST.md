# Recon 06 — Complete artifact manifest (Sprint 0 lane 01)

Generated: `2026-10-23T10:53:43Z`

- Install (read-only): `<KOTOR_INSTALL>`
- Project root: `<PROJECT_ROOT>`
- Manifest (local, gitignored): `artifacts/lanes/01-manifest/manifest.jsonl` (15,380 rows, 4,360,474 B, SHA-256 `0dbd840cd59fa321df85c0cdff6632722decec2e19814bee90c7f2bb50dbad91`)
- Script (local): `artifacts/lanes/01-manifest/build_manifest.py` (38,203 B, SHA-256 `9133ecf128d97d27387262b924f28318edb5ba35fe527cf246d9ec66ec453e68`)
- Summary JSON (local): `artifacts/lanes/01-manifest/summary.json` (enclosing document; SHA-256 verified by the post-write audit)

Only this summary is committed; the per-file manifest and script stay under `artifacts/` (gitignored).

## Result

**Manifest complete: true.** Every regular file under the install root was streamed through SHA-256: **15,380 files, 3,602,889,094 bytes (3.355 GiB)**.

No symlinks, junctions, reparse points or non-regular files were found; no path escaped the install root. Pre- and post-hash stat snapshots are identical, so the install's file list, sizes and mtimes are unchanged. The lane contains metadata only; no game bytes were copied.

## Files by kind

| Kind | Files | Bytes | Size share |
|---|---:|---:|---:|
| `data_bif` | 26 | 1,316,112,080 | 36.53% |
| `texture_pack` | 4 | 672,407,587 | 18.66% |
| `movie` | 61 | 606,082,480 | 16.82% |
| `stream_voice` | 13,799 | 548,135,774 | 15.21% |
| `stream_music` | 119 | 199,468,577 | 5.54% |
| `stream_sound` | 970 | 117,658,824 | 3.27% |
| `module_rim` | 234 | 72,466,498 | 2.01% |
| `rim` | 12 | 39,034,549 | 1.08% |
| `patch_erf` | 1 | 7,243,543 | 0.20% |
| `executable` | 4 | 6,577,152 | 0.18% |
| `lip_sync` | 123 | 5,625,868 | 0.16% |
| `talk_table` | 1 | 5,394,446 | 0.15% |
| `launcher_bitmap` | 5 | 2,015,368 | 0.06% |
| `library` | 6 | 1,671,680 | 0.05% |
| `launcher_audio` | 2 | 1,194,360 | 0.03% |
| `bitmap` | 1 | 829,688 | 0.02% |
| `key` | 1 | 569,239 | 0.02% |
| `audio_driver` | 4 | 392,192 | 0.01% |
| `config` | 5 | 8,367 | 0.00% |
| `launcher_config` | 1 | 770 | 0.00% |
| `save` | 1 | 52 | 0.00% |
| **Total** | **15,380** | **3,602,889,094** | **100.00%** |

## Files by extension

| Extension | Files | Bytes |
|---|---:|---:|
| `.wav` | 14,890 | 866,457,535 |
| `.rim` | 246 | 111,501,047 |
| `.mod` | 123 | 5,625,868 |
| `.bik` | 61 | 606,082,480 |
| `.bif` | 26 | 1,316,112,080 |
| `.bmp` | 6 | 2,845,056 |
| `.erf` | 5 | 679,651,130 |
| `.dll` | 4 | 1,318,400 |
| `.exe` | 4 | 6,577,152 |
| `.ini` | 4 | 6,955 |
| `.m3d` | 3 | 281,088 |
| `.asi` | 2 | 353,280 |
| `.vdf` | 2 | 2,150 |
| `.flt` | 1 | 111,104 |
| `.key` | 1 | 569,239 |
| `.tlk` | 1 | 5,394,446 |
| `.txt` | 1 | 84 |

## Top-level locations

| Location | Files | Bytes | Size share |
|---|---:|---:|---:|
| `data/` | 26 | 1,316,112,080 | 36.53% |
| `TexturePacks/` | 4 | 672,407,587 | 18.66% |
| `movies/` | 61 | 606,082,480 | 16.82% |
| `streamwaves/` | 13,799 | 548,135,774 | 15.21% |
| `streammusic/` | 119 | 199,468,577 | 5.54% |
| `streamsounds/` | 970 | 117,658,824 | 3.27% |
| `modules/` | 234 | 72,466,498 | 2.01% |
| `rims/` | 12 | 39,034,549 | 1.08% |
| `(install root)` | 11 | 20,689,693 | 0.57% |
| `lips/` | 123 | 5,625,868 | 0.16% |
| `launcher/` | 8 | 3,210,498 | 0.09% |
| `miles/` | 7 | 1,118,208 | 0.03% |
| `utils/` | 5 | 878,406 | 0.02% |
| `saves/` | 1 | 52 | 0.00% |

## Largest files

| Path | Kind | Bytes |
|---|---|---:|
| `data/models.bif` | `data_bif` | 954,052,730 |
| `TexturePacks/swpc_tex_tpa.erf` | `texture_pack` | 399,642,722 |
| `data/sounds.bif` | `data_bif` | 194,679,056 |
| `TexturePacks/swpc_tex_tpb.erf` | `texture_pack` | 116,034,130 |
| `TexturePacks/swpc_tex_gui.erf` | `texture_pack` | 111,598,685 |
| `TexturePacks/swpc_tex_tpc.erf` | `texture_pack` | 45,132,050 |
| `movies/01a.bik` | `movie` | 42,101,388 |
| `movies/31a.bik` | `movie` | 37,699,920 |
| `movies/50.bik` | `movie` | 25,986,776 |
| `movies/54.bik` | `movie` | 24,207,444 |

## Evidence artifacts hashed

| Path | Role | Bytes | SHA-256 |
|---|---|---:|---|
| `artifacts/ghidra/functions.jsonl` | `ghidra_functions_export` | 2,435,119 | `c75778f419bf281274c6a1db32350db4be3c9c64e64adabd0cbcac0947256ccc` |
| `artifacts/ghidra/functions-full.jsonl` | `ghidra_functions_full_export` | 17,810,231 | `c6ab51d18044bf8d17c040c7a922c9b28cb6731b8ef41f84e1a2d08c2366e03e` |
| `artifacts/inventory.json` | `artifact_inventory` | 85,278 | `4e3dede4dbb2be00d5d5eb0a1900407f83a051589f806122d3d86ca8ffc10aff` |
| `artifacts/action_table.json` | `action_table` | 142,526 | `49a6390a6e85cef1a2aca384902d1f895616e567c1deedf07e998a87e600e53b` |
| `artifacts/analysis/ghidra-handlers/coverage.json` | `handler_coverage` | 410,944 | `1174411802621d6d752f8bdc058172d43c049e28888fc44002d2653049a6c481` |
| `artifacts/analysis/ghidra-handlers/verification-independent.json` | `handler_verification_independent` | 5,795 | `73cd5cb2bac57985b9753385c7994644e85fdde9a7dfa8378da7fc8f0c7e4ef0` |
| `artifacts/analysis/ghidra-handlers/copy_functions.jsonl` | `handler_copy_functions` | 2,565,838 | `a4396cbc33db62d085bc2fce568d0a0a5b4a1e06725c5b5a98d54dd7d13f1345` |
| `artifacts/analysis/ghidra-handlers/new_functions.jsonl` | `handler_new_functions` | 720,947 | `d6d73fdce62e67b5053600b3d36f389367d0e21b9ebe91a7823fdd2bb5457480` |
| `docs/recon/03_GFF_COORDINATES.md` | `recon_gff_coordinates` | 9,187 | `17559f7ea73a0bd90c239f37462c224632d63fa0f042a146dc9331463acaabad` |
| `docs/recon/04_GEOMETRY_COORDINATES.md` | `recon_geometry_coordinates` | 7,466 | `8f1dc24c094e251a04011d41b45e0d714ee061ccd060b14e4d77992b405f5697` |
| **Total (10 files)** | | **24,193,331** | |

## Validation

- Containment checks passed: 15,390 (15,380 install files + 10 evidence files); path escapes: 0.
- Symlinks/junctions/reparse points skipped: 0; non-regular entries: 0; read errors: 0.
- Pre-scan stat snapshot: `b9339fe50a4f032ab4e02b3491a980abc6a039940ef4dd47216bc2b338621c4b` (15,380 files).
- Post-scan stat snapshot: `b9339fe50a4f032ab4e02b3491a980abc6a039940ef4dd47216bc2b338621c4b` (15,380 files).
- File list unchanged: true; sizes unchanged: true; mtimes unchanged: true.
- Manifest round-trip (reparsed rows): 15,380; digest match: true.
- Lane policy: expected 4 metadata-only files (`build_manifest.py`, `manifest.jsonl`, `summary.json`, `summary.md`), allowed extensions `.py`, `.jsonl`, `.json`, `.md`; binary files: 0; install-content hash matches: 0; lane cap 16,777,216 B.
- Outputs are confined to `artifacts/lanes/01-manifest`. The script's post-write audit rehashes every lane file, enforces the name/extension/size policy, and rejects any lane file whose hash matches install content.

## Notable gaps

- Zero-entry directories: Override, logs (no files).
- Override/ contains 0 files; no loose override mods are installed.
- saves/ contains only saves/steam_autocloud.vdf (52 B); no player save game is present.
- logs/ contains 0 files; no game log was captured in the install.

## Limitations

- SHA-256 covers the default data stream of each regular file; NTFS alternate data streams, ACLs and sparse-extent layout are not enumerated.
- Pre/post proof compares the file list, sizes and mtimes; every byte was read, but a rewrite that preserved both size and mtime could not be excluded by stat alone.
- Evidence hashes are point-in-time and become stale if those artifacts are regenerated.

## Reproduce

```
python -B artifacts/lanes/01-manifest/build_manifest.py
```

