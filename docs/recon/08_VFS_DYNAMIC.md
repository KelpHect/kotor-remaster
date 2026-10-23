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

## Unresolved

* Save-vs-module order and real save loading remain untested because this install has no save
  folder. Lane 05's synthetic save tests pass. The controlled off-screen boot stops at
  `MAINMENU.rim`; no path under `saves/` or `modules/*` is opened, and the harness does not drive
  character creation/save UI.
* The shipped install has no duplicate keys and no module `.mod`; duplicate precedence inside a
  table and `.mod` vs RIM are covered by synthetic archives only.
* The game rewrites `.\swkotor.ini` in its working directory (not the install in the controlled
  runs); probes must set and restore settings explicitly and run the install guard.

## Safety

Dynamic runs are windowed (`FullScreen=0`), launched suspended, parked off-screen, and run at idle
priority. `SetForegroundWindow` is replaced in-process and windows are shown with
`SW_SHOWNOACTIVATE`; `CreateWindowEx*` is parked before first show, `SetWindowPos`/`MoveWindow` are
rewritten to `-32000`, and a watchdog re-parks new instances. `max_render_onscreen=0` was sampled
throughout. See `tools/harness/offscreen.py`, `tools/harness/install_guard.py`, and the
shared-resource rules in `docs/SPRINT0.md`.

Full-desktop captures from the earlier lane (`sess_*.bmp`, `screen*.bmp`, `state_01/02`, movie
frames) were removed for privacy and are listed in
`artifacts/lanes/06-dynamic/incident/DESKTOP_CAPTURES_REMOVED.txt`.
