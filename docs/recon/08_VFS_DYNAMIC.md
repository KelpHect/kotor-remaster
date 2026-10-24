# Dynamic VFS verification (lanes 05/06)

Status: **verified** for the five-pass order, first-hit precedence, intra-pass archive order,
Override shadowing, and the texture-quality → pack mapping. Evidence comes from a user-owned local
process captured with Frida and from lane 05's synthetic resolver/tests. The install was not written
by the controlled runs.

## Method

Lane 06 captured the original process with hooks on the resource manager lookup, the per-pass
lookup, the archive loaders, and Win32 file opens. The compacted evidence is in
`artifacts/lanes/06-dynamic/logs/` and `cont/` (untracked). After lane 06 ended, the integrator
re-ran short probes off-screen with `tools/harness/offscreen.py` and in-process window parking; no
window took focus and no desktop frame was captured. Probe logs:

* `vfs_q2.jsonl` — Texture Quality 2, no shadow
* `vfs_q1.jsonl`, `vfs_q0.jsonl` — quality 1 and 0
* `vfs_q2_shadow_border2.jsonl` — quality 2 with a synthetic `Override/border2.tpc`
* `cont/logs/{baseline,fixture,dup2}_events.jsonl` — file/registry baseline, one synthetic tag-1
  archive, and two same-tier archives

The probe script is `artifacts/lanes/06-dynamic/scripts/vfs_probe.py`; the continuation probe is
`cont/scripts/fsreg_probe.py`. Both launch the profile suspended, install hooks, then resume; every
created window is moved to `(-32000,-32000)` and shown without activation. Synthetic files are
removed afterwards.

## Five-pass order (verified)

Every resource lookup runs the same pass sequence. The pass arguments are a list head plus a
filter; the two ERF-tagged passes share one list and differ by filter:

| step | list (relative) | filter | tier |
|---:|---|---:|---|
| 1 | `...cc` | 0 | directories (`Override`, streams, movies) |
| 2 | `...d0` | 1 | ERFs tagged 1 (`patch.erf`, `Override/textures.erf`) |
| 3 | `...d4` | 0 | RIMs (module `_a/_adx/_s`, `global.rim`, menus) |
| 4 | `...d0` | 2 | ERFs tagged 2 (module `.mod`, saves, `lips`, `players`, TexturePacks) |
| 5 | `...c8` | 0 | `chitin.key` + BIFs |

This matches lane 05's resolver (`artifacts/lanes/05-save-vfs/tools/kotor_vfs.py`) and its
`test_mounts_are_five_passes` / `test_pass2_beats_pass3_and_pass4` tests. In the captured path all
five passes are attempted; the result is the **earliest hit**, not the last.

## First-hit precedence (verified)

`border2` (TPC, type 3007) is a GUI texture present in `swpc_tex_gui.erf`.

* Baseline, quality 2: passes 1–3 miss; pass 4 (filter 2) hits; the resource manager returns the
  GUI pack table (`0x4597968` in that run). Pass 5 misses.
* With `Override/border2.tpc` copied from the GUI pack (identical bytes, lane-local only): the game
  opens `.\Override\border2.tpc` six times; pass 1 (directory, list `...cc`) hits; the returned
  table changes to the directory table (`0x447aec0`), and the later tagged-ERF pass misses.
  Removing the file restores the baseline.

This is direct engine evidence for the documented rule: directories beat tagged-1 ERFs, which beat
RIMs, which beat tagged-2 ERFs, which beat the KEY/BIF base. It also confirms that a loose Override
resource of type 3007 is indexed under the TPC extension.

`patch.erf` is loaded before RIMs and before the texture packs, consistent with pass 2. The four
boot-time ERF-family loaders are now named from the loader object's CExoString at **`this+0x20`**:

| order | archive | notes |
|---:|---|---|
| 1 | `OVERRIDE:textures` | optional tag-1 slot `Override/textures.erf`; absent in retail, empty table |
| 2 | `HD0:patch` | `patch.erf` |
| 3 | `TEXTUREPACKS:swpc_tex_tpa` | selected tier |
| 4 | `TEXTUREPACKS:swpc_tex_gui` | GUI textures |

`loader_rim` fires twice (`RIMS:GLOBAL`, `RIMS:MAINMENU`); `loader_key` once (`HD0:CHITIN`). The
file-open order at boot is: `chitin.key` + BIFs → `patch.erf` → `GLOBAL.rim` → selected
`swpc_tex_tp*` → `swpc_tex_gui` → `MAINMENU.rim` when the menu activates.

## Intra-pass order (verified with two same-tier archives)

Two synthetic tag-1 archives both contained `mainmenu/2047`: `Override/textures.erf` and a
re-serialized 97-entry `patch.erf` (lane 05 writer). Both were loaded; `Override/textures.erf` loaded
first (seq 92), `patch.erf` second (seq 104). The lookup hit **`patch.erf`**
(resman table `0x4667aa8` = the `HD0:patch` loader object) while the `OVERRIDE:textures` table
`0x4667b28` also contained the key but was never returned.

This is last-added-table-first within pass 2, and it **corrects lane 05's note** that
`Override/textures.erf` is searched before `patch.erf`. The synthetic 97-entry `patch.erf` booted
and served the lookup, which is the first retail-engine acceptance of the lane 05 writer (structural
acceptance at boot, not byte-compatibility). Fixtures were removed and the original `patch.erf`
restored (`0eecf9f9…`).

Pass 3 (RIMs, last-added-first) was measured the same way: a synthetic `GLOBAL.rim` (shipped 949
entries + `mainmenu/2002`) loaded first (table `0x4677778`, seq 114), `MAINMENU.rim` loaded second
(`0x46777f8`, seq 571), and the natural lookup returned **MAINMENU**. Pass 4 (tagged-2 ERFs) at
Texture Quality 0: a synthetic one-entry `swpc_tex_tpc` loaded first (`0x468c830`, seq 145),
`swpc_tex_gui` second (`0x468ccb0`, seq 156), and `border2/3007` present in both returned **gui**.
Both synthetic archives booted the menu, extending structural acceptance to the lane 05 RIM/ERF
writers.

## Texture quality → pack (verified)

| `Texture Quality` (ini) | pack opened | `gui` pack |
|---:|---|---|
| 2 | `swpc_tex_tpa.erf` | always loaded |
| 1 | `swpc_tex_tpb.erf` | always loaded |
| 0 | `swpc_tex_tpc.erf` | always loaded |

Only the selected tier plus `swpc_tex_gui.erf` is opened; `tpb`/`tpc` are not opened at quality 2.
The engine also probes `.mod`, `.nwm`, and `.sav` siblings for each pack before the `.erf`. The
four boot ERF loaders are the optional `Override/textures.erf`, `patch.erf`, the selected tier, and
the GUI pack.

## Filesystem and registry baseline (verified)

Three 20-second runs (baseline, one synthetic tag-1 archive, two synthetic tag-1 archives) logged
436 relevant opens each. 332 resolve through the profile symlinks into the install and are all
read-only (`GENERIC_READ`, `OPEN_EXISTING`); the only write-intent opens are 14
`.\swkotor.ini` opens (`GENERIC_WRITE`, `CREATE_ALWAYS`) in the profile. The install stat tree
(16,045 files/directories) is unchanged in every run, and the profile ini hash is unchanged (the
engine rewrites identical content with `FullScreen=0`, `Texture Quality=2`).

Registry exports before/after are byte-identical (`34f1f526…`). The only KotOR key is
`HKLM\SOFTWARE\WOW6432Node\BioWare\SW\KotOR` (`path`, `flavour=Retail`,
`LastInstalledVersion=1`, `language=0`); no HKCU or LucasArts KotOR key exists. The probe blocked 18
registry-write attempts per run (one AMD `HKIDs` create and 17 DirectInput device values) by
returning success without calling advapi32; none referenced a KotOR key. This is evidence that the
controlled runs did not persist those keys, not proof of what an unblocked run would do.

The earlier install-config change was therefore not reproduced by the controlled runs. The install
guard remains mandatory because a run launched with the install as its working directory could
write `swkotor.ini`; the restored file matches the manifest hash.

## Closing-pass measurements (2026-10-24)

The closing pass used pre-first-instruction Frida hooks (`0x4071a0`/`0x407230` lookups,
`0x40e990` `CExoKeyTable::AddKey`, ERF/RIM/KEY loaders and record reads) plus synthetic fixtures in
the lane profile. Exact events and fixture hashes are in
`artifacts/lanes/06-dynamic/vfs-precedence/`. All measured runs report integrity OK; install files
are unchanged and the saves/patch/ini were restored. A second elevated pass on 2026-10-24 added the
`runs2/` cases below.

* **Duplicate key inside one archive: first entry wins.** `patch.erf` (tag 1) `border2` entry 97 won
  over entry 98 (395 B read; 16,523 B ignored); `rims/mainmenu.rim` entry 1 won over entry 2; a
  module `.mod` `m41ad` entry 3 won over entry 4. The second `AddKey` returned 0 and the duplicate
  was never read.
* **`.mod` vs RIM (loaded-save path):** when a `.mod` is present, it is mounted *instead of*
  `_s.rim` (no `_s` loader). A mounted RIM (`rims/global.rim`) beats `.mod` because pass 3 precedes
  pass 4. `.mod` beats `lips/localization.mod` (same pass 4). This refines “`.mod` loses to every
  RIM”: for `_s` the RIM is not mounted at all.
* **Save vs module:** the save's nested module ERF loses to module `_s` RIM for a stored key
  (`module`/2014 resolved to `_s` entry 246; save entry 2 ignored), but wins over `.mod` (both pass
  4, save searched first; save entry 2, `.mod` entry 0 ignored). The save supplies area/git; `_s`
  supplies static resources. These are resolver-hit observations during extraction/load, not proof
  of playability.
* **Pass 3:** `_s` is searched before `global.rim` (last-added-first). **Pass 4:** save nested ERF
  > `.mod` > `LIPS:localization` > `HD0:players` > texture packs, last-added-first; `.mod` >
  `lips/localization.mod`. The `LIPS` vs `players` position is now measured directly: with a
  shared `m12ab`/3000 key, `runs2/lips-vs-players-2` returned `LIPS:localization` (pass event 483,
  resman 484) after mounting players (seq 404) and LIPS (416/418). **Pass 2** remains `patch.erf`
  before `Override/textures.erf`.
* **KEY duplicates:** a byte-patched profile `chitin.key` (25,836→25,837 records) with a duplicate
  `mainmenu`/3008 appended returned the first record's table index; the second `AddKey` returned 0.
  First KEY record wins, consistent with ERF/RIM.
* **Loose directories are flat, one level.** `FindFirstFile*` scans `.\override\*.*`,
  `.\MOVIES\*.*`, `.\STREAMWAVES\*.*`, `.\STREAMMUSIC\*.*` once and never enumerates a child
  directory. `MOVIES` honours its ini alias; `streammusic`/`streamwaves` opens use HD0 root paths
  and ignore their aliases. A top-level `streamwaves/border2.tpc` wins pass 1; a subdirectory-only
  copy is never opened and falls through to the texture pack. The earlier “sorted/recursive”
  assumption is withdrawn for this build.
* **Directory enumeration order is the OS order, not a user-space sort.** `runs2/enum-dirs-1`
  created `zz_lane06a.tpc`, `aa_lane06b.tpc`, `mm_lane06c.tpc`, `border2.tpc`; `FindFirstFile`
  returned `aa_lane06b.tpc`, `border2.tpc`, `mm_lane06c.tpc`, `zz_lane06a.tpc` (NTFS name order).
  The index follows enumeration order.
* **Pass-1 directory tables are last-added-first.** All four loose directories indexed the
  controlled `border2`/3007 key; `AddKey` order was Override (seq 18), `HD0:MOVIES` (175),
  `HD0:STREAMWAVES` (1170), `HD0:STREAMMUSIC` (1425), and the lookup at seq 3187 returned
  **`HD0:STREAMMUSIC`**. Pass 1 still wins over tags/RIMs/KEY; within pass 1 the last-added table
  wins.
* **Save-nested duplicate drop site is at conversion output.** `runs2/save-dup-1` appended a
  510-byte `module`/2014 duplicate (`VFSDUP-SECOND`) at index 3 of nested `m12ab.sav`. After Load,
  `gameinprogress/m12ab.sav` still had 4 entries, but `currentgame/m12ab.rim` had 3 (the duplicate
  gone) and `CURRENTGAME:m12ab` (AddKey seq 475) saw only those 3. The converter is not
  instruction-traced; the drop is proven before the `CURRENTGAME` table is built.

## Remaining inferred / open

* Fresh-module area-RIM intra-order (`_a`, `_adx`, `.rim`, `_s`) and `.mod` vs area RIM remain
  unmeasured: controlled New Game runs reach `RIMS:CHARGEN`, but the character-generation `PLAY`
  control did not activate under posted or hardware mouse input, arrow keys, Enter, or the
  number hotkeys (the sheet stayed on `PORTRAIT`). This is an input blocker, not a loader stall.
  `_s` > `global` and `MAINMENU` > `GLOBAL` are measured; the loaded-save path mounts only `_s` or
  `.mod` instead.
* `streamwaves` *audio* requests before gameplay remain unobserved; the directory itself, its
  one-level scan, and its pass-1 index are measured.
* Save playability/HUD/new-save/reload is verified separately at 1280×720
  (`docs/recon/10_SAVE_VFS.md`, `docs/recon/16_WINDOWED_1280.md`); this lane measured resolver hits
  during extraction/load.

The closing passes used the user-authorized focused windowed Load Game path; they were windowed
only, captured only the game window, and took no full-desktop capture. See
`artifacts/lanes/06-dynamic/vfs-precedence/summary.md`.

## Safety

Dynamic runs are windowed (`FullScreen=0`). The earlier off-screen runs were launched suspended,
parked off-screen, and run at idle priority. The user then explicitly authorized a focused
main-screen test: the closing passes used normal game-window activation/focus and input, with only
the game window captured and no full-desktop capture. No install guard overwrite, no
process-name-wide manipulation, and no repeated focus reclamation. See the shared-resource rules in
`docs/SPRINT0.md`.

Full-desktop captures from the earlier lane (`sess_*.bmp`, `screen*.bmp`, `state_01/02`, movie
frames) were removed for privacy and are listed in
`artifacts/lanes/06-dynamic/incident/DESKTOP_CAPTURES_REMOVED.txt`.
