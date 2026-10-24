# Save containers and VFS (lane 05)

Status: **structural parsing verified; retail save load, save-write, and reload verified at
1280×720 (2026-10-24). Fresh-module area-RIM order and streamwaves audio requests remain
unmeasured (P1).** Archive I/O and the five-pass resolver have synthetic tests and the bounded
dynamic measurements in `docs/recon/08_VFS_DYNAMIC.md`. Downloaded manual/autosave slots now exist
in the test profile; the original reference install remains separate.

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
`Override/textures.erf` first; the resolver and test now follow the measurement. Pass-1 loose
directories are also last-added-first: with `border2`/3007 present in Override, MOVIES,
STREAMWAVES, and STREAMMUSIC, the lookup returned `HD0:STREAMMUSIC` after those tables were added
in that order. The remaining intra-pass orders (RIMs, tagged-2 ERFs) are measured last-added-first
as well; see `docs/recon/08_VFS_DYNAMIC.md`.

## Dynamic confirmation

In-process hooks confirmed the five-pass order and first-hit precedence, Override beating a
tagged-2 texture pack (same `border2.tpc` payload), and the quality mapping:

| Texture Quality | pack |
|---:|---|
| 2 | `swpc_tex_tpa.erf` |
| 1 | `swpc_tex_tpb.erf` |
| 0 | `swpc_tex_tpc.erf` |

`swpc_tex_gui.erf` is always loaded. Details and logs: `docs/recon/08_VFS_DYNAMIC.md`.

## Engine save acceptance (2026-10-23)

The retail engine was driven with a profile-local Mesa 26.2.4 llvmpipe renderer and a vanilla save
from pcsavegames (`000004 - VANILLA`, `SAVEGAME.sav` SHA-256 `19149072…`, `END_M01AA`; validator
PASS with only the empty-`Mod_Tag` warning). Menu, four-slot listing, and the Load request all pass;
the engine extracts `currentgame/` and `gameinprogress/` and reaches its loading screen.

Two renderer defects had to be bypassed without touching the game or saves:

* `FUN_0047a2c0` at `0x0047a2c0` calls `glGetString(0x8874)` (`GL_PROGRAM_ERROR_STRING_NV`) and
  dereferences the result without a null check. Mesa returns NULL for the unsupported enum; the
  engine faults at `0x0047a2ce` (`CMP byte ptr [EAX],0`, `EAX=0`). The earlier ATI run faulted in
  `atioglxx+0xae71e7` at the same query. The probe replaces the NULL with an empty string.
* `GLU32.dll+0xf94a` (`gluScaleImage`, called from `gluBuild2DMipmaps`) faults on a 256×256
  `GL_LUMINANCE_ALPHA` image even though the data pointer is non-null; `ECX=0xd4f2c6f0` is used as a
  source-row pointer. The engine call site is `0x423e7c` in `FUN_00423d80`. The probe replaces
  `gluBuild2DMipmaps` with Mesa `glTexImage2D` plus `GL_GENERATE_MIPMAP`.

With those shims, the vanilla save leaves the loading screen only when sound is enabled; it then
renders a static Endar Spire space frame with no HUD. With sound disabled, the loading bar stalls at
~20% with zero disk I/O while the main thread renders at 74 fps. The modded downloaded saves
(`unk_m41xx`, empty `Mod_Tag`) reach a full loading bar but never leave the overlay. A later vanilla
Taris Hideout save (`000005 - TARIS`, module `tar_m02af`) and Taris Lower City (`000006 - TARISLC`,
`tar_m03aa`) are structurally accepted: the engine extracts them and mounts
`MODULES:<module>_s` + `CURRENTGAME:<module>`, but produces no frame: `GL_VIEWPORT` reads 0×0,
`glGenTextures` leaves engine texture id `0x7bb868` at 0, and the main thread loops in
`FUN_00423d80`. ESC/Return/Space/click do not change it. The renderer shims are not causal (a
no-op-GLU run loops identically).

The context lifecycle is now measured: Mesa's `wglMakeCurrent(pbufferHDC, ctx 0x1)` returns 0 and
clears the thread's current context; the engine normally re-binds the saved main pair
(`0x93010f80/0x1`). If that pair is stale during the load transition, the thread stays contextless.
A lane-local `wglMakeCurrent(lastHdc,lastHglrc)` restoration shim turns a forced loss back into
800×600 frames (`restore_make_current` ret 1, `restore_ok`, two successes). This 800×600 run could
not be re-driven at the time because the host foreground was owned by a protected
`swtor-origins.exe` (PID 13476); `AttachThreadInput` failed with `ACCESS_DENIED` (5), background
Load clicks never left the main menu, and an isolated desktop crashed on a no-context path. The
elevated 1280×720 run below later superseded it. Evidence:
`artifacts/lanes/06-dynamic/save-acceptance-taris/summary.md` and `.../cont2/summary.md`.

### Windowed 1280×720 acceptance (2026-10-24)

The retail engine was driven at a true 1280×720 windowed client on a 2560×1440 desktop with an
elevated foreground session and the profile-local Mesa renderer. Results:

* **Frame Buffer effects are the black-screen cause.** With `Frame Buffer=1`, the load transition
  creates a 120×120 offscreen target and Mesa's `wglMakeCurrent(pbufferHDC, ctx)` fails, leaving the
  main thread contextless (`GL_VIEWPORT` 0×0, `FUN_00423d80` loop). Web-confirmed fix: set
  `Frame Buffer=0` (and disable `Shadows`/`Soft Shadows`/`Grass`/`Anti Aliasing`). With these off,
  the 120×120 loop disappears, the viewport stays 1280×720, and the load reaches the HUD.
* **Taris load passes.** `000005 - TARIS` mounted `MODULES:tar_m02af_s` and
  `CURRENTGAME:tar_m02af`; the HUD (minimap, action icons, party) rendered at 1280×720.
* **Save write/reload passes.** In-game quicksave (F4) created `000000 - QUICKSAVE`
  (`savenfo`: `QUICKSAVE`, area `Taris - Hideout`, `last_module=tar_m02af`; nested module
  `tar_m02af`). Quickload (F5) reloaded the HUD at 1280×720. The slot parses with **0 errors,
  0 warnings** after the thumbnail step below.
* **Window size is stable.** The earlier shrink was the test harness's focus helper calling
  `SetWindowPos(..., 800, 600)`; removing that call keeps the main client at 1280×720 through the
  load and reload (`status-after-load` client `[0,0,1280,720]`).
* **Immediate row selection matters.** Delayed synthetic clicks only hovered; an immediate
  in-process down/up (`clickfast`) committed the Taris row and the Load action.
* **Save thumbnail residual.** The engine's screenshot resampler `FUN_0045dad0` faults at
  `0x0045db21` under Mesa when generating `Screen.tga` (source/dimension argument path). For the
  acceptance run the lane hook skipped the screenshot body and a 256×256 `Screen.tga` was
  synthesized from the HUD capture; the save itself and reload are unaffected. This is a
  renderer-compatibility residual, not a save-format defect.

Evidence: `artifacts/lanes/06-dynamic/save-acceptance-taris/cont2/runs/elev-load11-noshrink-20261024`
(Taris HUD) and `.../elev-save4-noshot-20261024` (quicksave/quickload; `summary.json` new slot,
`20-quicksave.png`, `22-quickload-14s.png`). The original six slots are byte-identical; the
install is untouched; only the new quicksave slot was added.

## Tests

The original lane run of `python tools/tests/run_all.py` recorded **33 tests, 32 pass, 1 skip,
0 failures**. The current suite records **47 tests, 46 pass, 1 skip, 0 failures/errors** (skip:
no real save in the reference install; the downloaded slots are checked separately below).
Generated fixtures live in `artifacts/tests/`.

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

* Save playability is verified for the Taris Hideout path at 1280×720 (load → HUD → F4 quicksave →
  F5 quickload). Not yet exercised: manual saves from the in-game menu, saves in other modules, and
  load of a save produced by this engine path in a fresh process. The vanilla `END_M01AA` space
  frame without HUD and the modded downloads that stall on the loading bar remain unexplained
  engine-load-transition residuals, not parsing failures.
* The save thumbnail is a renderer residual: `FUN_0045dad0` faults at `0x0045db21` under Mesa, so
  in-game saves require either a Mesa-compatible resampler shim or a clean-room screenshot
  replacement. The acceptance run skipped the screenshot and synthesized `Screen.tga`; the save
  data itself is unaffected.
* The writer is not byte-compatible; the retail engine accepted a synthetic `patch.erf` at boot,
  which is structural acceptance, not a save round-trip guarantee.
* Duplicate-in-archive, `.mod` vs RIM, save-vs-module, pass-3 `_s`-vs-global, pass-4
  save > `.mod` > `LIPS:localization` > `players` > texture-pack order, KEY duplicates, and flat
  loose directories were measured on 2026-10-24. First entry/record wins; a mounted global RIM
  beats `.mod`; `.mod` is mounted instead of `_s`; a save nested ERF beats `.mod` but loses to
  `_s`; the shared `m12ab`/3000 contest returned `LIPS:localization` over `HD0:players`; `MOVIES`
  uses its ini alias while `streammusic`/`streamwaves` use HD0 root paths. Exact logs:
  `artifacts/lanes/06-dynamic/vfs-precedence/` (`runs2/lips-vs-players-2`).
* The save-nested duplicate is dropped at the save→RIM conversion output: the nested
  `m12ab.sav` held 4 entries including a 510-byte `VFSDUP` duplicate at index 3, the converted
  `currentgame/m12ab.rim` held 3, and `CURRENTGAME:m12ab` was built from those 3
  (`runs2/save-dup-1`). The converter is not instruction-traced.
* Directory enumeration order is the OS order (`FindFirstFile` returns NTFS name order) and the
  engine does not sort; loose directories are flat and pass-1 tables are searched last-added-first
  (`runs2/enum-dirs-1`).
* Fresh-module area-RIM intra-order and `.mod` vs area RIM remain unmeasured: the
  character-generation `PLAY` control did not activate under posted/hardware mouse, arrows, Enter,
  or number hotkeys. This is an input blocker, not a loader stall. `streamwaves` audio requests
  before gameplay also remain unobserved; its directory and pass-1 index are measured.
* The earlier `lane06dup` null-name hook records remain invalid as duplicate evidence; the new
  synthetic AddKey/resman and conversion-output records supersede that gate.
