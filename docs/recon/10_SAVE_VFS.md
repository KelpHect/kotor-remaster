# Save containers and VFS (lane 05)

Status: **save layout, archive I/O, and the five-pass resolver are implemented and tested.** Dynamic
precedence and texture-quality selection are verified in `docs/recon/08_VFS_DYNAMIC.md`. A real save
folder is absent from the reference install, so real-save parsing is the one skipped test.

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
original padding, unused labels, or row order, and retail engine acceptance is unverified. It refuses
to write inside a game install.

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

`python tools/tests/run_all.py` (set `KOTOR_GAME_DIR` for real-install cases):
**33 tests, 32 pass, 1 skip, 0 failures.** The skip is `test_real_save_if_present`; the reference
install has no save folder. Fixtures are generated under `artifacts/tests/` and are entirely
synthetic.

## Open items

* No real save has been parsed; NFO/PT/GVT field semantics, autosave rules, and nested-module naming
  beyond the documented layout remain inferred.
* The writer is not byte-compatible and has not been accepted by the retail engine.
* Intra-pass table order for RIMs and tagged-2 ERFs is inferred from the model; a real duplicate
  resource in two same-tier archives is still needed.
* Save-vs-module order and `.mod` precedence lack a real corpus (no module `.mod`, no save).
* Loose-directory scan order is assumed sorted; engine enumeration could differ.
