# Save containers and VFS (lane 05)

Status: **structural parsing verified; retail save loading and reload remain unverified.** Archive
I/O and the five-pass resolver have synthetic tests and the bounded dynamic measurements in
`docs/recon/08_VFS_DYNAMIC.md`. Downloaded manual/autosave slots now exist in the test profile;
the original reference install remains separate.

## Save container

A save slot is a folder named `NNNNNN - NAME` (`000000 - QUICKSAVE`, `000001 - AUTOSAVE`, manual
slots from `000002`):

```
<slot>/
  SAVEGAME.sav      ERF tagged MOD V1.0; one nested <module>.sav per visited module
  savenfo.res       NFO GFF
  PARTYTABLE.res    PT GFF (party, gold, journal)
  GLOBALVARS.res    GVT GFF (positional category tables)
  Screen.tga        screenshot (manual saves)
  pifo.ifo          player info (autosave; may also exist elsewhere)
  CORRUPT.res       empty failed-unpack sentinel
```

Each nested module ERF holds `Module.ifo`, `Module.are` (NWM modules may omit it), and
`Module.git`, all GFF V3.2. ERF uses 24-byte keys and 8-byte resource records.

Synthetic fixtures (manual, autosave, corrupt, missing sidecar) parse and validate; a semantic
rewrite reproduces IFO entry, ARE rooms, GIT counts, party/gold/journal, GVT values, and inventory
counts with zero parser check errors. The writer is **structural only**: it does not preserve
original padding, unused labels, or row order. A synthetic 97-entry `patch.erf` built by this writer
was accepted by the retail engine at boot (menu loaded, duplicate lookup served); byte compatibility
is still unverified. It refuses to write inside a game install.

## Archive and VFS implementation

`tools/resources/` contains:

* `kotor_archive.py` — ERF/MOD/SAV, RIM, BIFF and KEY readers plus deterministic fixture writers
  that preserve entry order and duplicate keys.
* `kotor_gff_write.py` — GFF V3.2 writer/semantic re-serializer on top of the verified reader.
* `kotor_save.py` — save parser, structural state model, validator, and semantic writer.
* `kotor_vfs.py` — five-pass resolver with case-insensitive resrefs, first-entry-in-table and
  last-added-table-in-pass, traces, and shadowed hits.
* `build_dynamic_probes.py`, `vfs_measure.py` — probe manifest builder and hook-log comparator for
  dynamic measurement.

Important base-layer rule: BIF resource ids are full resids for nonzero BIF indices
(`gui.bif` ids are `1048576+`), not just the low 20-bit table index.

Real read-only archive counts match the manifest: `chitin.key` 25,836 keys / 26 BIFs,
`patch.erf` 97 entries, `modules/M12ab.rim` 3 entries, `rims/global.rim` 949 entries, texture packs
tpa/tpb/tpc 3,294 entries each, GUI 1,571.

## Verified lookups on the reference install

| key | winner |
|---|---|
| `1280x1024back` (3007) | pass 2 `patch.erf` |
| `m12ab` (2012) | pass 3 `m12ab.rim` |
| `k_pebo_skybox` (2010) | pass 3 `m12ab_s.rim` |
| `acbonus` (2017) | pass 3 `global.rim` over pass 5 (two hits) |
| `3dgui_a00001` (3007) | pass 4 `swpc_tex_tpa.erf` |
| `actions` (2017) | pass 5 `chitin.key` |

Synthetic precedence cases cover Override, `patch.erf` vs RIM, same-pass last-added-first, module
RIM vs `.mod`, texture pack vs `.mod`, save-only/module/save-vs-RIM, base-only, duplicate keys,
case-variant duplicates, independent types, and the first-launch loose-WAV quirk.

Measured pass-2 order: the engine loads `Override/textures.erf` first and `patch.erf` second, so
`patch.erf` is searched first under last-added-first. A two-archive synthetic test (`mainmenu/2047`
in both) returned the `patch.erf` table. This corrects the earlier resolver note that searched
`Override/textures.erf` first; the resolver and test now follow the measurement. The other
intra-pass orders (RIMs, tagged-2 ERFs) remain inferred.

## Dynamic confirmation

In-process hooks confirmed the five-pass order and first-hit precedence, Override beating a
tagged-2 texture pack (same `border2.tpc` payload), and the quality mapping:

| Texture Quality | pack |
|---:|---|
| 2 | `swpc_tex_tpa.erf` |
| 1 | `swpc_tex_tpb.erf` |
| 0 | `swpc_tex_tpc.erf` |

`swpc_tex_gui.erf` is always loaded. Details and logs: `docs/recon/08_VFS_DYNAMIC.md`.

## Tests

The original lane run of `python tools/tests/run_all.py` recorded **33 tests, 32 pass, 1 skip,
0 failures**. The skipped `test_real_save_if_present` checked the original install, which had no
save slot; it did not check the later downloaded slots. Generated fixtures live in `artifacts/tests/`.

The static recovery separately audited the two downloaded slots against source commit
`cf2803839f789bbf149fc0a1b848998714d8c5ee` of `FlorianGG/savesKOTOR`:

| slot | outer entries | nested modules | audited containers | errors / warnings |
|---|---:|---:|---:|---:|
| `000002 - Game1` | 21 | 9 | 52 | 0 / 7 |
| `000001 - AUTOSAVE` | 21 | 9 | 53 | 0 / 8 |

All ten files match the source blobs. The earlier six-module count was wrong. Both slots warn
about a missing party-leader flag and six absent/empty module tags; autosave additionally lacks
`SAVEGAMENAME`. No repair was applied. Evidence:
`artifacts/lanes/06-dynamic/cont3/snapshots/recovery-static-20261023/save-validation/summary.json`
and `source-provenance.json` in that recovery directory.

The current modules are `unk_m41ab` and `unk_m41ad`; their 7 and 10 room-model sets have installed
payload candidates. This is direct-reference coverage, not full transitive compatibility or proof
that the saves are unmodified retail output. Engine listing, load request, confirmed module startup,
new-save creation and reload are separate verification gates.

## Open items

* Both downloaded saves pass implemented structural checks, but NFO/PT/GVT runtime semantics,
  full dependency compatibility, original-engine loading and save/reload acceptance remain open.
* The writer is not byte-compatible; the retail engine accepted a synthetic `patch.erf` at boot, but
  this is structural acceptance, not a save/round-trip guarantee.
* Pass-2 order is measured (`patch.erf` before `Override/textures.erf`); pass-3 RIM and pass-4
  tagged-2 orders are also measured last-added-first (MAINMENU over synthetic GLOBAL; gui over
  synthetic tpc). Duplicate-in-archive, `.mod` vs RIM, and save-vs-module remain in a closing pass.
* The interrupted duplicate-in-archive probe did not look up its discriminating `lane06dup` key;
  its 677 null-name hook records do not identify a winning duplicate. That gate remains open.
* Save-vs-module and `.mod`/RIM precedence need an observed module-load path. Previous boot-only
  runs did not enumerate saves; the newly authorized windowed foreground test uses normal Load Game
  before any further native-call experiments.
* Loose-directory scan order is assumed sorted; engine enumeration could differ.
