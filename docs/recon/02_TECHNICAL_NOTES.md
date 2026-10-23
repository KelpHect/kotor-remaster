# Recon 02 — Technical notes from the research lanes

Date: 2026-10-23. These are consolidated facts and constraints for execution, not a second master plan.
Sources are linked at the end; confidence is noted where it matters.

## 1. Prior art and what it means

| Project | Stack / license | Coverage and status | How we use it |
|---|---|---|---|
| reone (+ modawan fork) | C++17, SDL2/OpenGL 3.3, GPL-3.0 | Deepest open engine core; modules, renderer, VM, dialogue, combat, saves; upstream dormant 2025, fork active 2026 | Behavioral reference and test index only; never copy code or prose |
| KotOR.js (KobaltBlu) | TypeScript/Three.js, GPL-3.0 | Browser/desktop client; many systems; early-stage completeness; active 2026 | Parser behavior and test ideas; external reference only |
| xoreos / xoreos-tools | C++, GPL-3.0 | Shared Aurora engine and format tools; partial KotOR gameplay; tools active | External CLI oracles (GFF/ERF/NCS/TLK); no linking |
| PyKotor / Holocron | Python, LGPL-3.0+ | Widest modern format library and tools; active; no runtime engine | External oracle/CLI; LGPL obligations if ever distributed with the engine |
| OpenKOTOR Kaitai specs | Kaitai, MIT | GFF, ERF, MDL, DLG, NCS, BWM, etc. | Reusable format specifications with attribution |
| kotorblender | Python/Blender, GPL-3.0 | Mature MDL/MDX import/export, LYT/PTH, lightmaps | Asset authoring and v0 conversion oracle |
| KotOR-Unity / NorthernLights | C#/Unity, GPL/AGPL | Archived/dormant, incomplete | No direct reuse |
| Apeiron | UE4 fan remake, closed | Shut down by Lucasfilm (2018) | Risk evidence, not a technical source |

No project claims a complete 1:1 playthrough. The hard gaps everywhere are content completion,
save compatibility, combat/script edge cases, UI fidelity, and media codecs.

## 2. Format facts that change implementation

### Containers

- **KEY V1** (`KEY V1  ` header; community "V1.1"): 12-byte file records observed as
  `{size u32, nameOffset u32, nameLength u16, drives u16}`; a u32 name-length reading is invalid
  here. 22-byte key records; the final 4-byte field carries the BIF index in bits 20–31 and the
  table index in the low 20 bits. Our parser reconciles all 26 BIFs exactly.
- **BIFF V1**: 20-byte header, 16-byte resource records at the variable-table offset, fixed table
  after it. Payloads are uncompressed. This build reconciles with 16-byte records; a claimed extra
  V1.1 flag dword is not present and must not be added without evidence.
- **ERF/MOD/SAV**: 160-byte header in the specification; this build's `patch.erf` validates with
  `entryCount` at `0x10`, key list at `0x14`, resource list at `0x1C`; 24-byte keys, 8-byte resource
  records. The parser tries both this and the common NWN layout and records which validates.
- **RIM**: 20-byte header (`RIM V1.0`, reserved, count at +12, table offset at +16), 32-byte entries
  `{resRef[16], type, id, offset, size}`. Our parse of 234 modules is exact.
- **NCS** is **big-endian**, unlike every other format: `NCS V1.0` + `0x42` + u32 size.

### Data formats

- **GFF**: V3.2 only. 56-byte header, 12-byte structs, 12-byte fields, 16-byte labels; top-level
  struct type `0xFFFFFFFF`. Field type 16 = Orientation (4 floats), 17 = Vector (3 floats),
  18 = StrRef. Community docs sometimes invert 16/17. Wire order is verified on a real GIT/ARE
  pair: field records are `{type, label, data}` (type first), field rows are reached through the
  field-index table, and list values are byte offsets to `{u32 count, u32 struct indices[]}`.
  Section counts for struct/field/label are entries; field-data/field-index/list-index lengths are
  bytes. See `docs/recon/03_GFF_COORDINATES.md`. Preserve unknown fields and struct IDs.
- **2DA**: K1 ships binary `2DA V2.b` (tab/NUL separated, u16 row offsets); ASCII V2.0 is also valid.
  Empty cell is not `"****"`; row label is not necessarily the row index.
- **TLK**: V3.0, 20-byte header, 40-byte entries (flags, 16-byte sound resref, volume/pitch variance,
  text offset/length, float sound length). The table in this install has 49,265 entries.
- **SSF**: KotOR V1.1 is 28 StrRefs with no filename table; do not use the NWN layout.
- **LIP**: `LIP V1.0`, float duration, u32 frame count, then `(float time, u8 viseme 0–15)`.
- **TPC**: 128-byte header; data size (0 = raw), alpha-test float, width/height at 0x08/0x0A,
  encoding at 0x0C (1 gray, 2 RGB/DXT1, 4 RGBA/DXT5, 0xC BGRA), mip count at 0x0D; TXI text is
  appended after mip data (compute the offset, do not scan).
- **MDL/MDX**: no magic; the first u32 must be 0. K1 version is detected from geometry-header function
  pointers. 12-byte header, offsets exclude it. MDX vertex/animation arrays. 30 FPS animations,
  4-weight skinning, hooks, lights, emitters, supermodels.
- **BWM** variants: 2016 = WOK (area), 2052 = DWK (door), 2053 = PWK (placeable); all carry
  `BWM V1.0`. Preserve triangles, surface IDs, adjacency, perimeter, and the AABB tree.
- **PTH** is GFF; the shipped key is `Path_Conections` (typo preserved).
- **TGA** (type 3) is the base-BIF texture format in this install; **TPC** (3007) lives in the
  texture-pack ERFs and `patch.erf`. Do not confuse them.
- **WAV** SFX: some carry a ~470-byte prefix before `RIFF`; probe signatures, not extensions.
- **BMU** is MP3 with a `BMU` prefix. **BIK** is Bink 1; FFmpeg's decoder is a viable oracle.

### Legal/format sources
The BioWare Aurora PDFs are proprietary documentation mirrored for modding: read them, do not
redistribute. The MIT Kaitai specs are the reusable baseline.

## 3. NWScript VM and action table

- **K1 has 772 actions** (K2 877, NWN 848). The names are not stored in the executable; the action
  id is the declaration order in the shipped `nwscript.nss` (772 prototypes, extracted locally).
- **Verified in this build:** `InitializeCommands` at **`0x0054c960`** (8,295 bytes) allocates
  `0xC10` = 772 × 4 at object+`0x0C`, zeroes it, writes 677 slots, and tail-jumps to
  `0x005cd2c0`, which writes 97 more. Total **774 writes / 772 unique slots** (id 156 written twice;
  later `0x534340` wins; id 709 written twice with the same handler). **All 772 slots are mapped.**
  The earlier 757 count was a parser artifact: 15 offsets were printed in decimal and slot 0 had no
  offset, so the hex-only regex missed them. Action 0 = Random, 32 = ActionEquipItem, 77 = abs,
  771 = YavinHackCloseDoor.
- **Dispatcher verified at `0x0052c0d0`** (missing from the initial Ghidra function list): checks
  id < 772, loads `[ecx+0xC][id]`, rejects null/out-of-range with `-2002` (`0xFFFFF82E`), then calls
  the handler with `(id, arg)`. The bytes at VA `0x0052c0d0` were read directly. The service vtable
  at `0x7450e4` holds `0x0054c960` at +4 and `0x0052c0d0` at +8; both pointers were found in the
  executable file. ExecuteCode's ACTION path targets this service (call-site observation is from
  the companion lane; the vtable pointers are independently verified).
- **Coverage pass:** an isolated project copy defines and decompiles all 440 `LAB_` handler targets
  plus the dispatcher (**441/441, 0 failures**). The copy has 9,057 internal functions (8,616
  baseline + 441) and 350 external imports (9,407 total `getFunctionCount`). The 125 pre-existing
  `FUN_` handler targets were verified but not renamed; runtime dispatch has not been executed.
  This does not change the 772-slot mapping.
- **NCS**: 13-byte header `NCS V1.0` + `0x42` + **big-endian** u32 size; all multi-byte values are
  big-endian. Opcodes `0x01–0x2D` (CPDOWNSP, RSADD, CPTOPSP, CONST, ACTION, arithmetic, jumps,
  JSR/RETN, DESTRUCT, SAVEBP/RESTOREBP, STORESTATE, …); `0x42` is a size pseudo-op. Type qualifiers:
  3 int, 4 float, 5 string, 6 object, 0x10–0x13 effect/event/location/talent, plus binary and
  engine-struct pairs. Stack slots are 4 bytes; arguments are pushed in reverse; the callee pops
  them; non-void returns are pushed. Object invalid is `0x7F000000`; recursion limit is 8.
- There is no VM sleep. `DelayCommand` snapshots a script situation; `ActionWait`/`AssignCommand`
  use the action queue; `ExecuteScript` is nested and depth-limited. Hooks are ResRefs from IFO/ARE
  and object templates, not VM constructs.
- K1 differs from NWN/K2 mainly in the action table and four engine structs; K1 NCS is not
  compatible with K2. There are no array/GETREF opcodes in K1.
- Tooling: **DeNCS (MIT)** is the best current decompiler; xoreos `ncsdis` is solid while
  `ncsdecomp` is partial; PyKotor (LGPL) has reader/writer/compiler and a known-failure corpus;
  KotOR.js and reone (GPL) contain VMs; `nwnsc` compiles NWN/K1 source. No public tool proves a
  round-trip of every shipped NCS. A community vanilla-source corpus exists for comparison.
- The `swkotor1-reconstruction` repository matching this binary is likely derived material: use it
  only as an address cross-check, never as source or spec text.
- Strategy: extract every NCS with PyKotor/xoreos, hash it, disassemble all, decompile with DeNCS
  and a second tool, diff outputs, recompile, and publish per-file exact/normalized/failed status.
  Build the independent VM; hook `ExecuteCode`/`RunCommand` in the original and compare
  IP/opcode/SP/BP/stack types/values/action args/returns, normalizing RNG, time, and object IDs.

## 4. Engine/rendering facts

- Original renderer: OpenGL 1.x-era fixed function, lightmapped static geometry, per-vertex lighting,
  stencil shadow volumes, alpha test/blend, fog, CPU/billboard particles. KotOR is CPU-scene-bound,
  not modern-GPU-bound.
- Texture packs A/B/C contain identical resrefs (3,294 TPC each); GUI pack 1,570 TPC; `patch.erf`
  contains the 1280×1024 UI set. The selection rule is unmeasured.
- Coordinate conversion to glTF: KotOR Z-up RH → glTF Y-up RH, `(x, y, z) → (x, z, -y)`, conjugate
  quaternions. Do not swap axes without fixing winding.
- Classic path should preserve gamma-space lightmap × vertex-color behavior; the Remaster path can
  use sRGB/linear, PBR approximations, shadow maps, SSAO, etc., behind a toggle.

## 5. Dynamic analysis notes

- Current build fingerprint: PE32 i386, base `0x400000`, entry `0x2fb38d`, no `.bind`, no
  `steam_api` import, RELOCS_STRIPPED, not Large Address Aware, sections `.text/.rdata/.data/.rsrc`.
- Frida 17.24.3 supports ia32 attach/interception on this host. MinHook 1.3.4/Detours 4.0.1 build a
  32-bit harness DLL. x32dbg is the live debugger. RenderDoc does not handle this legacy GL path;
  use a 32-bit apitrace and Mesa llvmpipe for stable reference captures.
- High-value hooks: single NWScript action dispatcher, resource manager (`CExoKeyTable`), input
  (`CExoInputInternal::GetEvents`), combat round (`CSWSCombatRound`), save/load, movie player,
  sound source. The assert strings in the binary provide anchors.
- RNG: MSVC LCG code near RVA `0x2FC451` in this build (`imul 0x343FD`, `add 0x269EC3`, state
  `[eax+0x14]`); verify by pattern and prologue before hooking.
- Feasible determinism: fixed save + config + scripted input + recorded RNG + virtual clock; compare
  semantic logs, save fields, and perceptual captures. Not feasible: bit-exact full-game replay.
- One shared game process per suite, serialized; clean snapshot per test; named-pipe control; JSONL
  evidence with exe/DLL/ini/save hashes, driver/Mesa versions, frame IDs.

## 6. Game-system rules map

Ownership split (critical for implementation):
- **Data-owned:** class tables, feats, skills, spells, item dice/properties, BAB/saves/defense,
  XP tables, encounter difficulty, camera styles. Parse and apply 2DA/GFF; do not hardcode.
- **Engine-side:** d20 attack vs Defense, critical confirmation, saves, damage application,
  deathblow/unconscious/party-wipe, XP split, difficulty scaling, merchant pricing, transitions,
  minigame framing. These need RE specs and differential tests.
- **Script-owned:** most Force effects (`k_inc_force`), dialogue traversal side effects, alignment
  and quest XP, encounter hooks, cutscene choreography.

Key documented rules and test targets:
- 30-point buy; scores cost 8/10/13/16 for 15–18; cap 18 at creation; +1 at levels 4/8/12/16/20.
  Skill caps: class L+3, cross-class (L+3)/2. Force DC = 5 + level + WIS + CHA. XP = 500·L·(L−1).
  Alignment 0–100, starts 50. Party gains 80% XP; three active party members.
- Combat: d20 + attack vs Defense (10 + armor + capped Dex + effects), natural 1/20 rule,
  threat confirmation, ×2 default crit, Fort/Ref/Will saves, armor Max Dex; difficulty scales
  final enemy damage by −50%/+50% on Easy/Difficult (round down).
- Round length is believed to be ~3 s but is inferred, not documented; no initiative stat is
  documented. Treat timing and hit-die/Force-point randomness as open RE questions.
- Merchants: UTM `MarkUp`/`MarkDown`/`StoreGold`/`WillNotBuy`/`StoreList`; rounding and theft
  handling need runtime measurement. K1 has workbench upgrades, not TSL-style crafting.
- Dialogue: DLG branch conditions/scripts, VO/Sound, camera angle, skippable, journal Quest/
  QuestEntry and XP; scripts drive alignment/quest/XP. LIP provides 16 visemes.
- Areas: ARE/GIT/IFO, UTT/UTD/UTE/UTW, transitions (`LinkedTo`, `TransitionDestin`, load screens),
  encounters (single/continuous, min/max, respawn/reset), camera lists; spawn timing is open.
- Minigames: Pazaak (fixed AI decks, 3 sets), swoop (3 tracks, gears, boost pads, laps), turret;
  Ebon Hawk serves as the hub. AI/RNG internals need measurement.
- Bug-parity candidates: Anywhere Menu Glitch, Displaced Load Zone, Spawn Warps, Hotshot,
  The Routine, Fake Level Up, item duping; vanilla Rancor double XP, Saul revive, Yuthura XP loop,
  Dak Vessar respawn. Classic mode should reproduce them; fixed mode should opt out.

Sources: official KotOR manual (Steam CDN), StrategyWiki system pages, DeadlyStream 2DA/script
notes, PyKotor/xoreos GFF/format docs, K1 Community Patch, KotOR speedrun guides. See lane D's
source list in the program log; formulas above are H/M confidence as noted in that lane.

## 7. Save/load and VFS precedence

### Save container

- Slot folder naming: `NNNNNN - NAME`; `000000 - QUICKSAVE`, `000001 - AUTOSAVE`, manual slots from
  `000002`. The folder is a staging directory during a save.
- `SAVEGAME.sav` is an ERF tagged `MOD V1.0`. It contains one nested `<module>.sav` ERF per visited
  module (resource type 2057), plus `REPUTE` (FAC), `AVAILNPCn` (UTC), `INVENTORY` (INV/UTI), and
  occasionally a stray PC.
- Each per-module save holds exactly `Module.ifo` (IFO), `Module.are` (ARE), `Module.git` (GIT);
  NWM modules skip ARE.
- Sidecars: `savenfo.res` (NFO), `PARTYTABLE.res` (PT), `GLOBALVARS.res` (GVT), `Screen.tga`.
  Autosaves add `pifo.ifo`, omit the screenshot, and put a loadscreen resref in `SCREENSHOT`.
  A failed unpack leaves an empty `CORRUPT.res` sentinel.
- All are GFF V3.2; ERF uses 24-byte keys and 8-byte resource records.

State ownership: module entry point/clock/tokens/player list in IFO; live creatures/doors/
placeables/triggers/encounters/stores/cameras in GIT; static area in ARE; party roster and
`PT_MEMBERS`/`PT_NPC_AVAIL`/`PT_GOLD`/journal in PARTYTABLE; globals in GLOBALVARS (900 bools,
500 bytes, 100 locations, 5 strings, positional); equipment/inventory inside the party/creature
snapshots; difficulty is the INI setting plus per-encounter `DifficultyIndex` resolved through
`encdifficulty.2da`.
Save lifecycle anchors: `CSWSModule::SaveModuleStart` / `SaveModuleFinish`,
`StallEventSaveGame`, `DoPCAutosave`, `CERFFile::ImportFiles`; load uses `LASTMODULE`,
`CopyGameToFutureGame`, `LoadModuleStart`. The biggest trap is `UseTemplates=0`: saved GIT objects
are flattened snapshots and must never be re-templated from `.utc/.utd` files.
Known hazards: an external `<module>.rsv` shadows saved state with no provenance check; a cached
module IFO with a corrupt `EventQueue` can make the save unloadable; mod removal can leave dangling
templates/scripts; TSLPatcher-style appends preserve indices, whole-file replacements do not.

### VFS precedence

The engine performs five first-match-wins passes over key tables (it does not branch by type):

1. Directories: `Override/`, plus top-level `streammusic/`, `streamwaves/`, `movies/`.
2. ERFs tagged 1: `patch.erf`, `Override/textures.erf`.
3. RIMs: active module `_a.rim`, `_adx.rim`, `_s.rim`; `rims/global.rim`;
   `mainmenu.rim`/`chargen.rim` while active.
4. ERFs tagged 2: module `.mod`, saved module state, `lips/*.mod`, `players`, `TexturePacks`.
5. `chitin.key` and its BIFs.

Within a pass, the table added last is searched first; within a table the first duplicate
`(resref, type)` wins; resrefs are 16-byte ASCII, case-insensitive, lowercased.
Module mount order: `players`; `lips/<root>_loc.mod`; `lips/localization.mod`;
`rims/<root>_a.rim`; `rims/<root>_adx.rim`; `modules/<root>.mod` else `modules/<root>_s.rim`;
then `CURRENTGAME:<root>` (RIM on a fresh load, save ERF on a loaded game). A `.mod` loses to every
RIM, including `global.rim`; a save is not a global tier and only shadows resources it stores.
Texture packs are session ERFs below module archives; `tpa` is highest quality, `tpb` medium,
`tpc` low — the quality-to-archive mapping still needs measurement. One quirk: a loose flat `.wav`
in `streamwaves/` can beat Override on the first launch, but Override moves to the head after a
game start/load.

Dynamic tests to measure: Override vs `_s.rim`; `.mod` vs `_a.rim`; `patch.erf` vs `_s.rim`;
BIF vs TexturePack vs Override at each quality; RIM vs saved module state; duplicate keys in one
ERF; case-variant resrefs; two TSLPatcher mods; synthetic save round-trip through PyKotor.
Instrument `CExoResMan::GetResource`/`FindResource` to log table/pass.

## 8. Clean-room and license constraints

- Do not copy decompiled pseudocode, identifiers, comments, or structure into engine code.
- GPL-3.0 projects (reone, KotOR.js, xoreos, kotorblender) are behavioral references only.
  PyKotor is LGPL-3.0+; use it as an external tool unless LGPL compliance is acceptable.
- MIT Kaitai specs may be reused with attribution.
- No game assets, strings, meshes, textures, audio, or save data in the repository. Asset caches are
  local and gitignored.
- No DRM circumvention. Analysis is performed on a user-owned 1.0.3.0 build; if a wrapped build is
  analyzed, observe only a legitimately running process. Public release requires IP counsel and a
  neutral name/branding.

## 9. Sources

- Kaitai specs: https://github.com/OpenKOTOR/bioware-kaitai-formats
- PyKotor: https://github.com/OpenKOTOR/PyKotor
- reone: https://github.com/seedhartha/reone
- KotOR.js: https://github.com/KobaltBlu/KotOR.js
- xoreos / xoreos-tools: https://xoreos.org/ , https://github.com/xoreos/xoreos-tools
- kotorblender: https://github.com/seedhartha/kotorblender
- MDLOps: https://github.com/ndixUR/mdlops
- BioWare/Aurora specs mirror: https://github.com/xoreos/xoreos-docs/tree/master/specs
- Frida: https://frida.re/docs/stalker/ ; https://frida.re/
- MinHook: https://github.com/TsudaKageyu/minhook ; Detours: https://github.com/microsoft/Detours
- apitrace: https://github.com/apitrace/apitrace ; Mesa for Windows: https://github.com/pal1000/mesa-dist-win
- FFmpeg Bink: https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/bink.c
- Ghidra 12.1.4 docs: https://www.ghidradocs.com/12.1_PUBLIC/
- Torlack NCS spec (archived): http://web.archive.org/web/20040905133822/http://www.torlack.com/nwndata/ncs.html
- xoreos NWScript bytecode article: https://xoreos.org/blog/2016/01/12/disassembling-nwscript-bytecode/
- DeNCS: https://github.com/KotORPublicDomain/DeNCS
- PyKotor NCS: https://github.com/OpenKOTOR/PyKotor
- nwnsc: https://github.com/nwneetools/nwnsc
- Vanilla KotOR script source corpus: https://github.com/KOTORCommunityPatches/Vanilla_KOTOR_Script_Source
- K1SE script-table extensions: https://www.nexusmods.com/kotor/mods/1852
- Reconstruction repo (derived material; address cross-check only): https://github.com/mattlgroff/swkotor1-reconstruction
- Rakata save deep dive: https://synchro.codeberg.page/rakata/internals/save_deep_dive.html
- Rakata resource system: https://synchro.codeberg.page/rakata/formats/resource_system.html
- Rakata save format: https://synchro.codeberg.page/rakata/formats/save/
- PyKotor save model: https://github.com/OpenKOTOR/PyKotor
- KSE: https://github.com/nadrino/kotor-savegame-editor
- TSLPatcher notes: https://deadlystream.com/topic/5188-tooltsl-patcher-tlked-and-accessories/
- Widescreen/INI context: https://www.wsgf.org/article/star-wars-knights-old-republic
- Prior-art / legal context: see lane outputs summarized in the master plan.
