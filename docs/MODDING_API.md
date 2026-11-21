# Modding API v1

Status: **loader implemented and tested headless; not yet wired into engine startup** (see
[What is not done](#what-is-not-done)). Sprint 17 lane `s17-04-modding`, gate S17 item 4.

A mod is data. The engine loads no native code from a mod, a mod never contains engine code, and
this repository never contains a mod's game-derived files. A mod lives outside the game install and
the install is never written to (ADR 0003).

The public surface is one header, `engine/src/kotor/Mods.h`, plus `Install::mountMods` in
`engine/src/kotor/Formats.h`.

## Layout

```
<mods directory>/
  load_order.txt        optional
  hd-ui/                one directory per mod, named after the mod id
    mod.ini             required
    override/           optional: whole-resource replacements, flat
    2da/                optional: table patches
```

Directories whose name starts with `.` or `_` are skipped, which is how a mod is parked without
deleting it. A directory without `mod.ini` is reported as a warning and skipped. `override` and
`2da` are matched in any letter case.

## mod.ini

`key = value` lines. `#` or `;` starts a comment line. Keys are case-insensitive. A key may appear
once. Keys this version does not know are ignored, so a later version can add keys.

| Key | Required | Value |
|---|---|---|
| `id` | yes | 1-32 characters of `a-z`, `0-9`, `_`, `-`. Must equal the directory name. |
| `version` | yes | `1`, `1.2` or `1.2.3`. Compared numerically. |
| `api` | yes | Whole number. This engine loads `1` only. |
| `name` | no | Display name. Defaults to the id. |
| `game` | no | `any` (default), `kotor1` or `kotor2`. |
| `author`, `description` | no | Free text. |
| `requires` | no | Comma-separated ids, each optionally `id >= version`. The other mod must be enabled, and loads first. |
| `after` | no | Comma-separated ids. Each loads first if it is enabled; it need not be. |
| `conflicts` | no | Comma-separated ids. This mod is rejected while one of them is enabled. |

```ini
id = hd-ui
name = HD UI
version = 1.4
api = 1
game = kotor1
requires = base-fixes >= 2.1
after = widescreen
```

## Which mods load, and in what order

1. Without `load_order.txt`, every valid mod is enabled, in id order.
2. With `load_order.txt` (one id per line, `#` comments), only the listed mods are enabled, in the
   listed order. An id that is not a valid mod is an error.
3. `requires` and `after` then move a mod later, only as far as needed.

**A later mod wins over an earlier one.**

A mod is rejected, with an error naming it, when: its manifest does not parse (the error gives file
and line); its directory is not named after its id; its `api` is not 1; its `game` is the other
game; a table patch in `2da/` does not parse; a `requires` is not enabled or is too old; a
`conflicts` is enabled; or it is in a `requires`/`after` cycle (a mod that lists itself is a cycle).
A rejected mod is not mounted, and neither is any mod that requires it. The other mods still load,
including a mod that only lists a rejected mod in `after`. `ModSet::ok()` is false when there
is any error; the caller decides whether to start anyway.

## override/

Each file replaces the resource with the same name and type everywhere: it wins over the install's
`Override`, `patch.erf`, module archives, texture packs and the KEY/BIF archives. File names are
case-insensitive. The directory is flat, like the game's own `Override`; sub-directories are
ignored with a warning. Files with an extension the engine has no resource type for are ignored.

Resolution order, first match wins:

1. Enabled mods' `override/`, last-loaded mod first.
2. Loose files: the overlay root's `Override`, then the install's `Override`.
3. `patch.erf`, `Override/textures.erf`.
4. Active module archives (`.mod` over `_s.rim` over `.rim`).
5. Other ERF/RIM layers: lips, texture packs.
6. `chitin.key` and its BIF archives.

`Install::find` reports a mod's file as `mod:<id>/override/<file>`.

## 2da/

`2da/<table>.2da` is a patch in `2DA V2.0` text form, applied to the table the engine would
otherwise load (which may itself be a whole-file replacement from `override/`):

- Rows are matched by row label, columns by column name, both case-insensitive.
- A non-empty cell replaces the cell. An empty cell or `****` leaves it alone.
- A row label or column name the table does not have is appended.
- Patches from several mods apply in load order, each on top of the last.
- If nothing else provides the table, the first patch is the table.

A patch file is ASCII `2DA V2.0`. Write `""` or `****` for a cell to leave alone, and quote a value
that contains a space. An optional `DEFAULT:` line before the column names is accepted and ignored.

Both ways of reading a table see the patches: `Install::table` returns the patched table, and
`Install::find` for a patched table returns it as ASCII `2DA V2.0` bytes (source
`mod:2da/<table>.2da (patched)`), so code that parses the bytes itself gets the same result. Use a patch rather than a whole file in `override/` whenever two mods might touch
the same table: patches compose, whole files do not.

## Overlaps

`ModSet::overlaps()` lists every resource two enabled mods both provide, with winner and loser:
`shared.txi` for an override file, `2da:appearance` for a table patch. An overlap is information,
not an error. Two table patches that touch different rows overlap and both take effect.

## C++ surface

```cpp
#include "kotor/Mods.h"

ke::kotor::Install install(gameDir);
const auto mods = ke::kotor::ModSet::discover(modsDir, install.profile().kind);
for (const auto& problem : mods.errors()) { /* report problem.mod, problem.message */ }
install.mountMods(mods);          // an empty ModSet unmounts
mods.fingerprint();               // "base-fixes@2.1.0;hd-ui@1.4.0"
```

`ModSet::discover` does not throw for a bad mod; it records the error. `ModManifest::parse` throws
`FormatError`. `fingerprint()` is meant to be stored in a save so a load can tell the mod set
changed.

## Compatibility promise

Within `api = 1`: the manifest keys above, the layout, the resolution order and the patch rules do
not change meaning. New optional keys and new optional directories may be added. Anything else
needs `api = 2`, and an engine rejects a mod whose `api` it does not know.

## Tests

`ctest -R kotor_mods` (`engine/tests/test_mods.cpp`). It needs no game install and no window: it
writes its own mods and a stand-in install (an empty `chitin.key` and a placeholder executable)
under the system temp directory, and checks manifest parsing, load order, every rejection above,
override precedence, table patching, unmounting, and that the install directory is unchanged
afterwards (same files, same sizes).

## What is not done

- **Engine startup does not call `mountMods` yet.** There is no `--mods <dir>` flag and no default
  mods directory. Until that lands, the API is reachable only from code and tests.
- Saves do not record `fingerprint()` yet.
- Not in v1: `dialog.tlk` string patches, script (NCS) hooks, GFF field patches, TSLPatcher
  `changes.ini` import, mods packed as a single archive. The existing `Tlk::merge` is the intended
  basis for string patches.

## To check on a PC with the game installed

The headless test cannot cover these; they need a real install (and the last two a GPU):

1. Build with MSVC (`scripts\build.bat msvc-release`) and run `ctest --preset msvc-release -R
   kotor_mods`. The test was only built with GCC on Linux.
2. `kotor_engine --install <dir> --vfs-test` still passes: the loose-file index and
   `Install::table` were touched.
3. With a real install: a mod `override/appearance.2da` wins over the BIF copy, and a mod
   `override/` file wins over the same name in `patch.erf` and in a module `_s.rim` (the stand-in
   install has no archives, so only mod-over-`Override` is tested headless).
4. A `2da/` patch against a real table such as `appearance.2da` or `feat.2da` shows up in game.
5. A texture replaced through a mod's `override/` (`.tga`/`.tpc`) renders.
6. Load time with a few hundred override files is not visibly worse.
