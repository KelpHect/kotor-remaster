# Dynamic VFS verification (lanes 05/06)

Status: **verified** for the five-pass order, first-hit precedence, Override shadowing, and the
texture-quality → pack mapping. Evidence comes from a user-owned local process captured with Frida
and from lane 05's synthetic resolver/tests. The install was not modified.

## Method

Lane 06 captured the original process with hooks on the resource manager lookup, the per-pass
lookup, the archive loaders, and Win32 file opens. The compacted evidence is in
`artifacts/lanes/06-dynamic/logs/` (untracked). After lane 06 ended, the integrator re-ran short
probes off-screen with `tools/harness/offscreen.py` and in-process window parking; no window took
focus and no desktop frame was captured. Probe logs:

* `vfs_q2.jsonl` — Texture Quality 2, no shadow
* `vfs_q1.jsonl`, `vfs_q0.jsonl` — quality 1 and 0
* `vfs_q2_shadow_border2.jsonl` — quality 2 with a synthetic `Override/border2.tpc`

The probe script is `artifacts/lanes/06-dynamic/scripts/vfs_probe.py`. It launches the profile
suspended, installs hooks, then resumes; every created window is moved to `(-32000,-32000)` and
shown without activation. The synthetic Override file is removed afterwards.

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

`patch.erf` is loaded before RIMs and before the texture packs, consistent with pass 2. The
loader/file-open order observed at boot is:

`chitin.key` + BIFs → `patch.erf` → `GLOBAL.rim` → selected `swpc_tex_tp*` → `swpc_tex_gui` →
`MAINMENU.rim` when the menu activates.

## Texture quality → pack (verified)

| `Texture Quality` (ini) | pack opened | `gui` pack |
|---:|---|---|
| 2 | `swpc_tex_tpa.erf` | always loaded |
| 1 | `swpc_tex_tpb.erf` | always loaded |
| 0 | `swpc_tex_tpc.erf` | always loaded |

Only the selected tier plus `swpc_tex_gui.erf` is opened; `tpb`/`tpc` are not opened at quality 2.
The engine also probes `.mod`, `.nwm`, and `.sav` siblings for each pack before the `.erf`, and
loads four ERF-family archives at boot (patch, selected texture pack, GUI pack, plus one further
tagged ERF/empty table we could not name from the loader hook).

## Unresolved

* **Intra-pass table order** (last-added table first) is inferred from the model and synthetic
  fixtures. A real duplicate resource inside two same-tier archives is needed to prove it; the
  shipped install has no duplicate keys and no module `.mod`.
* The fourth boot-time `loader_erf` call has no filename in the hook; correlating all four with
  file opens leaves one unnamed tagged archive/table. It does not affect the precedence result.
* Save-vs-module order and real save loading remain untested because this install has no save
  folder. Lane 05's synthetic save tests pass.
* The game rewrites `swkotor.ini` on exit; probes must set and restore settings explicitly.

## Safety

Dynamic runs are windowed (`FullScreen=0`), launched suspended, parked off-screen, and run at idle
priority. `SetForegroundWindow` is replaced in-process and windows are shown with
`SW_SHOWNOACTIVATE`; a watchdog re-parks any new instance. See `tools/harness/offscreen.py` and the
shared-resource rules in `docs/SPRINT0.md`.
