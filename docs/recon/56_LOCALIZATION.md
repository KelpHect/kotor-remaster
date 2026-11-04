# Recon 56 - localization: TLK language and localized paths

Status: Sprint 13 implementation note. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## TLK language

`dialog.tlk` is **TLK V3.0**, 20-byte header:

```text
0x00 "TLK "
0x04 "V3.0"
0x08 u32 language id       (shipped: 0 = English)
0x0C u32 entry count       (49,265)
0x10 u32 text table offset
```

`Tlk` now exposes `language()`. `Localization::load(-1)` uses the TLK's own
language; an explicit `--language <id|name>` accepts English, French, German,
Italian, Spanish, Polish, Russian, Czech, Hungarian, Portuguese, Chinese,
Japanese or Korean. A requested language loads `dialog_<id>.tlk` /
`dialog_<name>.tlk` when present, otherwise `dialog.tlk` must match or loading
fails with a message naming both languages. A failed explicit load leaves the
previous language intact.

The shipped install has one English TLK; the gate asserts:
`language=0`, `entries=49265`, and TLK entry 38167's sound resref
`c_drdastro_bat1`.

## TSLPatcher TLK merges

Sprint 14 adds `Tlk::merge(base, patch, name, replaceCount)` and
`Localization::load(Install&, ...)`:

- `Override/replace.tlk` replaces the first `replaceCount` StrRefs (all patch
  entries) and keeps the base text/sound for empty patch fields;
- `Override/append.tlk` appends after the base and preserves every base index;
- the two patches merge in that order, through the same `Override` VFS root
  used by the 2DA/GFF path;
- a DLG `StrRef` resolves through the merged table.

Gate: `tlk_merge_*` (5 assertions) and `tlk_vfs_*` (6 assertions) in
`docs/evidence/s14/localization.log`.

## Localized voice paths

The voice convention is:
`streamwaves/<module>/<speaker>/<resref>.wav` for `n<module><speaker>...`
resrefs (e.g. `nm01aatras02000_` -> `streamwaves/m01aa/tras02/`), with flat
`streamwaves/<resref>.wav` and `streamsounds/<resref>.wav` fallbacks.
`Localization::voiceFile` probes those paths (no full-tree scan). The gate
asserts the module/speaker/filename and decodes
`NM01AATRAS02000_.wav`: **32,000 Hz, mono, 177,872 frames**.

## Localized LIPs

`lips/` contains **117 `<module>_loc.mod`** archives plus
`lips/localization.mod` = **118 localized LIP archives**. `Install::addLips`
mounts all `lips/*.mod`; the gate parses `nglobebant00102_` from
`localization.mod` and asserts a non-empty timeline. The full LIP corpus
remains 18,206 resources in 123 archives.

## Gate

`--localization-test` asserts language selection/retention, names and bad ids,
TLK entry/sound, voice path components and PCM decode, localized LIP archive
count, a localized LIP resource, and the GameSession language. Evidence:
`docs/evidence/s13/localization.log`.
