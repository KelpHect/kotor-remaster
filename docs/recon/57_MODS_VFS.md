# Recon 57 - mods, Override precedence and 2DA merges

Status: Sprint 13 implementation note. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Five-pass first-match VFS

`Install::find` now searches explicit passes and records the winning table in
its optional `source` string:

| Pass | Source | Notes |
|---:|---|---|
| 1 | flat `Override/`, `movies/`, `streamwaves/`, `streammusic/` | one directory level only; overlay entries indexed last and searched first |
| 2 | `patch.erf`, `Override/textures.erf` | patch searched first |
| 3 | active module `_a.rim`, `_adx.rim`, `_s.rim` | `_s` last-added-first |
| 4 | module `.mod`, `<stem>.rim`, `lips/*.mod`, texture packs | `.mod` replaces `_s`; lips before texture packs |
| 5 | `chitin.key` + BIFs | first duplicate record wins |

Within a pass the last-added table is searched first; within a table the first
duplicate `(resref, type)` wins. Resrefs are case-insensitive and lowercased.
Loose files are mapped by extension (`2da` 2017, `utd` 2042, `gui` 2047,
`lip` 3004, `bik` 2063, `wav` 4, ...). `setOverlayRoot` lets tools and tests
place an Override profile outside the install without mutating it.

### Verified pass behavior on this install

- `end_door05` UTD resolves from `modules/end_m01aa_s.rim` (pass 3).
- `abilities` GUI resolves from `patch.erf` (pass 2) over the BIF copy.
- `gui_click` WAV resolves from the KEY/BIF (pass 5) in a fresh `Install`.
- A temporary `Override/appearance.2da` beats the BIF table, then the BIF
  returns when the file is removed.
- A temporary `Override/end_door05.utd` beats the `_s` RIM.
- A temporary `Override/abilities.gui` beats `patch.erf`.
- Copying `lips/localization.mod` to `modules/end_m01aa.mod` makes its
  `nglobebant00102_` LIP win over `lips/localization.mod`; removing the `.mod`
  restores the lips source.

`rims/global.rim` is present but is **not mounted for area loads**: its 2DA
copies reference models absent from the shipped BIF set (mounting it dropped
93 entity meshes and created 61 model warnings, failing the scene assertion).
This is a deliberate compatibility guard, documented rather than silently
regressing the baseline.

## TSLPatcher-style 2DA merge

`TwoDa::merge(base, patch)`:

- appends patch-only columns to the base and fills existing rows with blanks;
- overwrites existing rows only for non-empty, non-`****` cells;
- preserves base rows/order and appends new rows in patch order.

The gate uses `engine/tests/fixtures/vfs/merge_base.2da` and
`merge_patch.2da`: row 0 value overwritten to 99, row 1 untouched, new column
`added`, and row 2 appended with value 30 / added Q. TSLPatcher preserves row
indices by appending rather than renumbering; whole-file replacement remains
the fallback when a mod ships a complete table.

## Gate

`--vfs-test --vfs-fixtures engine/tests/fixtures/vfs` asserts pass-3/2/5
sources, loose Override over BIF/RIM/patch, `.mod` over lips, BIF fallback,
and all merge behaviors. Evidence: `docs/evidence/s13/vfs.log`.
