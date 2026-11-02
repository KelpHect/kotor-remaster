# Binary P1 verification (lane s1-07-binary, hydralysis / Hydra Engine h5)

Status: **complete for the four P1 binary handoff items.** All claims verified against the sealed
master view of target `kotor` (Hydra Engine 12.3-hydra.5) with build SHA-256
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6` quoted on every address.
Queries used `ghidra_query` actions `decompile/scan/writes/xrefs/function/status` with
`options.as = "s1_07"` (read-only on master; no `prepare`/`import`; no `runtime` — that belongs to
lane s1-06). Raw `disasm`/`strings` ran read-only against the install exe. Transcripts:
`artifacts/lanes/s1-07-binary/queries/TRANSCRIPT.md`. Evidence DB: claims **#11–#22** (this lane is
the only `evidence_add` writer). Prior related claims: #6–#8 (sprint0-recheck).

## 1. N-group mesh capability — VERIFIED (closes the P0 handoff "synthetic multi-group mesh" binary side)

Mesh data header (per mesh node): `+0x100` = face-count array ptr (count `+0x104`),
`+0x118` = group offsets array ptr, `+0x10c` = group handle array ptr, `+0x11c` = group count
(offsets/handles guarded by `+0x11c`). All three loops are bounded by a group-count **field**, not a
constant, and index the arrays per group:

* Relocation `FUN_004a0c00` (body `[4a0c00,4a0e23]`): `0x4a0c5e add dword ptr [ebx+0x100],edi`
  (guarded by `+0x104` at `0x4a0c56`), `0x4a0cad add dword ptr [ebx+0x118],edi` and
  `0x4a0cc3 add dword ptr [ebx+0x10c],edi` (guarded by `+0x11c` at `0x4a0ca3/0x4a0ca9`), then a
  per-group loop `0x4a0ce0–0x4a0d7b`: alloc `counts[g]<<1` bytes (`call 0x23ee0` at `0x4a0d0a`,
  size from `[ebx+0x100]+g*4` at `0x4a0cfa`), copy `(counts[g]>>1)` dwords from the relocated
  `offsets[g]` (`0x4a0ce4/0x4a0d0f`), tail `0x4a0d68 mov ecx,[ebx+0x11c]; 0x4a0d75 cmp eax,ecx; 0x4a0d7b jl 0xa0ce0`.
* GPU draw `FUN_0046f900`: `0x46f967 mov ebp,[esi+0x104]`; loop `0x46f990–0x46f9d4` reads
  `offsets[g]=[[esi+0x118]+edi*4]` (`0x46f990/0x46f996`), `counts[g]=[[esi+0x100]+edi*4]`
  (`0x46f999/0x46f99f`), draws via `call 0x25ab0` at `0x46f9ad`, tail `0x46f9cb inc edi; 0x46f9cc cmp edi,ebp; 0x46f9d4 jl 0x6f990`.
  Same loop in `FUN_00477830`: `call 0x25ab0` at `0x477a9d`, bound `[edi+0x104]` at `0x477ac1`,
  tail `0x477ac7 inc ebx; 0x477ac8 cmp ebx,eax; 0x477aca jl 0x77a80`.
* CPU skin `FUN_00487570`: outer loop bound `+0x11c` (`0x487741 mov ecx,[eax+0x11c]`, tail
  `0x4879e4 mov ecx,[eax+0x11c]; 0x4879ed inc ebp; 0x4879ee cmp ebp,ecx; 0x4879f4 jl 0x87760`),
  per group `offsets[g]` (`0x487760/0x487766`) and `counts[g]` (`0x487775–0x47777e`) drive the u16
  face-index loop.

**Conclusion:** the code is N-group capable by construction; all 76,703 shipped MDX blocks are
`1|1|1` (docs/recon/15), so N>1 is latent. The remaining P0-handoff piece — a *synthetic engine load*
with divergent counts — is renderer verification (P1/P4, lane s1-06/harness), not a shipped-code
gap; binary side closed. Evidence #11, #12, #13.

## 2. Supermodel child/super chain — VERIFIED (binary side closed; visual confirmation stays with s1-06)

* Clip lookup `FUN_004822a0` (`[4822a0,482630]`): scans the **local** array (`+0x58` ptr, `+0x5c`
  count) ascending with `__stricmp(entry+8, name)` (`0x4822ba–0x4822d0`); on miss recurses into the
  `+0x64` super link (`0x482470` et al.); only a terminal link (`+0x64==0`) additionally tries the
  fallback name locally; miss anywhere returns 0. → duplicate names resolve to the child's own
  definition; ancestor-only clips resolve up the chain; missing clips return 0. Evidence #14.
* Playback `FUN_00485bd0` (`[485bd0,48608f]`): first reuses an **active same-name instance**
  (`[esi+0xb0]` list, stricmp at `0x485c40`, match at `0x485c51` → reuse at `0x485c57`, flag `0x400`
  → set `0x401` return 1 at `0x485c6e–0x485c84`); else searches `+0x64` (call `0x4822a0` at
  `0x485d53`) then `+0x58` (at `0x485d6d`); total miss → `return 0` with no state change. Evidence #15.
* Query `0x484d20` (vtable method; DATA xrefs `0x741284`/`0x7419a4`; ret 0xc): active list `+0xb0`/
  `+0xb4` first (`0x84d40–0x84d65`, match `0x84db1` returns entry, out1=`[entry+0x10]`,
  out2=duration `+0x50`), then `+0x64` (`call 0x822a0` at `0x84d70`), then `+0x58` (`0x84d81`);
  miss → out2 = `0xbf800000` (−1.0f) at `0x84de3`, out1 = −1.0f at `0x84d91`, return 0. Evidence #16.
* Supernode binding: hierarchy clone `FUN_00449200` copies signed16 `[src+2]` → runtime `+0x28`
  (`0x49397/0x4939b`, `0x493c7/0x493cf`, `0x493f7/0x493fd`); binder `FUN_00483d80`
  (`[483d80,484250]`) skips children with `+0x28 == −1`, otherwise scans the animation-side child
  array with wraparound `(i+local_c) % count` and compares runtime `+0x28` against the animation
  node's signed16 `+2` (`0x483eda movsx eax,word ptr [eax+2]; 0x483ede cmp ebx,eax; 0x483ee0 je`),
  recursing into the match; no match → child/subtree skipped (`0x483ee7 jmp 0x8422a`), no bind-pose
  reset or name fallback at node level. Binding is by **supernode**, not raw index — consistent
  with `artifacts/lanes/10-coords-open/cont/misc3/supermodel_remap.json` (86/86 shared definitions
  differ; 6.9% raw-index alignment). Evidence #17.

**Conclusion:** duplicate (child wins / active instance reused), ancestor-only (chain walk), and
missing (return 0 / duration −1, no state change) clips are all handled as described in
docs/recon/12; visual output confirmation of a synthetic chain remains a dynamic (s1-06) item.
Binary side closed. Evidence #14–#17.

## 3. Camera `+0x38` — VERIFIED (closes the static side of the P0 handoff "dialogue-camera object")

* Whole-program `writes offset=0x38`: 265 `[reg+0x38]` stores; in the camera region
  0x6b0000–0x6e0000 the only plain store is the ctor `0x6bb6f8 mov [esi+0x38],ecx`
  (ecx = ctor arg from `0x6bb6f1 mov ecx,[esp+0x24]`); vtable `0x756ae8` is referenced only from the
  ctor (`0x6bb695`, `0x6bb756`). The other nearby `+0x38` writes are flag ops on other classes
  (`0x6c052d` OR in `FUN_006c03b0`; `0x6de0fd/0x6de149` OR/AND 0x40 in `FUN_006de0e0`).
* Value chain: the three camera-creation reads of controller `+0xc4` (`0x62bf4e`, `0x6320c9`,
  `0x632127`: `mov ecx,[esi+0xc4]; push ecx; call 0x6412f0`) feed factory `FUN_006412f0`
  (`0x641362 mov edx,[esp+0x1c]` … `0x64136f call 0x6bb670`), i.e. into the ctor store above.
* `writes offset=0xc4`: 57 stores program-wide; in the dialogue region only `0x630244`
  (`FUN_0062fed0`, 0xc24-byte controller ctor, body `[62fed0,6303a5]`, EBX=0 from `0x62feed xor ebx,ebx`)
  and `0x63e695` (`FUN_0063e530` ctor, vtable `0x7516c8`, EBX=0 from `0x63e560 xor ebx,ebx`).
* Mode reads in evaluator `FUN_006bb7b0`: `[ESI+0x38]` at `0x6bb90c` (`test edx,edx; jz 0x6bc090` —
  mode-0 path), `0x6bc67d`, `0x6bc99f` (`jne 0x2bca52`), `0x6bcc32` (`jne 0x2bcced`), `0x6bcfe8`
  (`je 0x2bd053`), `0x6bd5e5`, `0x6bd81f` (`jne 0x2bd91f`), plus `[EDI+0x38]` at `0x6bcf1f`.
  (Note: recon-12's read addresses `0x6bbde1/0x6bc098/0x6bc49f` read `+0x34`, the close-up flag, in
  this h5 view.)

**Conclusion:** camera `+0x38` is written only at construction from controller `+0xc4`, which both
controller ctors zero and nothing else writes → `+0x38 == 0` for every shipped camera and every
mode-nonzero branch is unreachable in shipped data (legacy/disabled). Caveat: the scans cover direct
`[reg+off]` accesses; stores through an aliased pointer would not appear (tool-reported coverage
limit). The live in-process observation of the camera object in a dialogue/cutscene scene remains
with lane s1-06. Binary side closed. Evidence #18, #19, #20.

## 4. MDX GPU skinning consumers + aux4 — VERIFIED (closes the P0 handoff aux4 / GPU-consumer items)

* Binder `FUN_0046a2f0` (`[46a2f0,46a3bd]`): `"qbone_ref_inv"→+0x1b8`, `"tbone_ref_inv"→+0x1c4`,
  `"boneconstantindices"→+0x1d0` (`0x46a39e lea eax,[edi+0x1d0]`), `"weights"→+0x19c`.
* GPU consumers (palettes) read qbone/tbone and upload to `program.env`: `FUN_0048a520`
  (`0x48a999 mov edx,[edi+0x1b8]`, `0x48a932 mov edx,[edi+0x1c4]`), `FUN_0048c650` (`0x48c9cf`,
  `0x48c973`), `FUN_0048d040` (`0x48d41c`, `0x48d3c0`, `0x48d9f2/0x48da14/0x48da24 [edx+0x1b8]`).
  Per-bone env index `3*map+0x12` (`0x48aa1e lea esi,[eax+eax*2+0x12]`; same lea at `0x48ca59` in
  0x48c650, `0x48d487` in 0x48d040), three vec4 uploads per bone via target `0x8620`,
  `call [0x7bb850]`/`[0x7bb7ac]` at `0x48aa4e/0x48aaaf/0x48ab16` → `program.env[3*map+0x12..0x14]`,
  map 0..16 ⇒ **env[18..68]**; loop bound `[edi+0x1b4]`, qbone row `+0x10`, tbone row `+0xc`.
  `0x48a633–0x48a63c`: `program.env[16] = (0, 2, 3, 1)`.
* Shader side: 8 `!!ARBvp1.0` programs contain `PARAM boneArray[51] = {program.env[18..68] };` at
  rva `0x391352, 0x392705, 0x393252, 0x393931, 0x394668, 0x3951d8, 0x396088, 0x397918`, skinning via
  `DP4/DP3 … boneArray[A0.x + 0..2]` with weights `attrib[1]`, bone ids `attrib[4]`
  (bound by `FUN_00478f50`, per docs/recon/15).
* aux4 (`+0x1d0`): `writes offset=0x1d0` → 48 stores program-wide; mesh-header-class sites only
  binder `0x46a39e` (address-of), reloc `0x4a0222/0x4a022b`, ctor `0x44acda`, dtor `0x448ef8`,
  copy/reset `0x489c8a/0x489cbb`. Register-based `[reg+0x1d0]` reads in the render/consumer paths
  `FUN_0046f900`, `FUN_00477830`, `FUN_00487570`, `FUN_0048a520/0x48c650/0x48d040`: **zero**.
  CPU skin uses the inline `+0x1ac` list and node2bone `+0x1dc` (claim #6).

**Conclusion:** GPU skinning consumers are qbone/tbone via `program.env[18..68]`; aux4 has no
consumer — parsed, relocated, copied, freed, dead on PC (corpus: 537 aux4 blocks of exporter scratch,
docs/recon/15). Closed. Evidence #21, #22.

## Open items carried

* **Closed 2026-11-02:** fresh-module area-RIM order (closure addendum §A, claims #23–#25) and the
  dialogue-start camera path (§B, claims #26–#28). The synthetic 2-group mesh and supermodel chain
  were verified live in lane s1-06 (docs/recon/25); the only live residue is the chargen/dialogue
  input path, not a binary ambiguity.
* Live in-process observation of a *dialogue scene* (the camera object itself) remains with lane
  s1-06 on hardware; the shipped path is bounded to `+0x38 = 0`.
* Scan coverage limit: aliased-pointer stores to `+0x38`/`+0xc4`/`+0x1d0` would not appear in the
  `writes` scans (stated by the tool itself).

## Evidence index (all build 0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6)

| claim | evidence_id | item |
|---:|---|---|
| #11 | ev_db61351e9c35bee25626a60043f98c445a48e1a80f78cfd3896b0cacd2a78f7f | N-group relocation FUN_004a0c00 |
| #12 | ev_e61d304f011a366f14cae8736d573af610ff0bf9558a9d10072f8a5942f44bf8 | GPU draw loops 0x46f900/0x477830 |
| #13 | ev_25f6ca849e6a5ebd8b327a6e584f10266ff9ba8d2fc071498022da8c5204a192 | CPU skin loop 0x487570 (+0x11c) |
| #14 | ev_d7786d852308ececdfc304c01d17788163a32f1ea67693b201ba2fd232d6c0ca | clip lookup 0x4822a0 |
| #15 | ev_07464bd5a0477b3537ffbee6077d25126cc101fab4394fd2636d0640e6faa8eb | playback 0x485bd0 |
| #16 | ev_6e6f708cf6ddbb49a9b7e8e5e1d4c36ff380304faac06c3f7bab01dfa61f4a06 | query 0x484d20 |
| #17 | ev_8ad82d152c5b99d017b6575a1ad895cfe9ccc3c5df83d471a47d6c0b2b27284c | supernode binding 0x449200/0x483d80 |
| #18 | ev_c6f86c14845cf5ac97ad848eafe03bb1a6552b1df984575e04e24615a417588b | camera +0x38 ctor-only write |
| #19 | ev_3cc236b47cea1dedca7798f07d8bd0377fb3c0ecff785bff6f3188fe5fa5f214 | controller +0xc4 zeroed |
| #20 | ev_55464e0054a33884e4ed7736c8d594ba6c1f3920495c73e93918d57c9fb79a7e | mode-nonzero branches unreachable |
| #21 | ev_b6c1d328ed67cd6a30c0c225e1bb8bd1ecfa63193d65929d1b960ace8bb6ac8b | GPU skin env[18..68] + shaders |
| #22 | ev_36d79876b511cf9ef2d618b0ff3930e0a5f24f7be83634edc324155a2a089902 | aux4 no consumer |

---

# Closure addendum (integrator follow-up, agent `s1_07b`, read-only on the sealed master)

Scope: statically close, as far as the binary allows, the two input-blocked dynamic items of
`docs/recon/22_DYNAMIC_P1.md` §6 — (A) the fresh-New-Game area-RIM mount order, (B) the
dialogue-start camera path. All queries `ghidra_query` on target `kotor` with `options.as =
"s1_07b"`; build SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`
quoted on every address. Raw transcripts: `artifacts/lanes/s1-07-binary/queries-closure/`.
Evidence claims **#23–#28**.

## A. Fresh-module area-RIM mount order (New Game path) — VERIFIED statically

Entry chain (all addresses build `0290889e…74e6`):

1. Main-loop handlers call the thunk `FUN_004ae860` (`0x4ae860 mov ecx,[ecx+4]` /
   `0x4ae863 jmp 0xbabb0`) from `0x401cf3` (after the `DAT_007a39fc+8` non-null check,
   `0x401ce3–0x401cf3`) and `0x404757`.
2. `FUN_004babb0` (`[4babb0,4bb335]`, module transition handler): request object
   `puVar10 = *(DAT_007a39fc+0x14)`; kind `puVar10[1]`: `1`/`3` with `puVar10[3]==0` →
   `FUN_004c9050` at `0x4bb0e8` (loaded-save path, builds `CURRENTGAME:<mod>`);
   `puVar10[3]==puVar10[2]` → `FUN_004c5880`; **else → `FUN_004c5720(puVar10[2],puVar10[3])`
   at `0x4bb142`** (new/different module).
3. `FUN_004c5720` (`[4c5720,4c587a]`): `*(param_1+0x1c8)==0` is the **new-game** branch
   (default `0x7f000000`), else `*(param_1+0x40)`; both call the mounter
   **`FUN_004c44d0(param_1+0x5c)`** at `0x4c57c8` (`0x4c57c2 lea eax,[esi+0x5c];
   0x4c57c5 push eax; 0x4c57c8 call 0xc44d0`) and `0x4c5805`. (`FUN_004c9050` also calls it at
   `0x4c9132`; `FUN_004c68a0` at `0x4c69b4`.)

Mount order inside `FUN_004c44d0` (`[4c44d0,4c4b7d]`, resman `DAT_007a39e8`), disasm-cited:

| # | site | action |
|---|---|---|
| 1 | `0x4c44fb call 0x8820` | mount caller's full table `param_1` (`TAG:name`; `:` split at `0x4c450c`) from the pass-3 ERF list (`FUN_00408820 = FUN_00407830(name,3)`) |
| 2 | `0x4c458f/0x4c45d1/0x4c4621/0x4c465f` | `CURRENTGAME:` create pass-2 (`0x8800 = FUN_00406e20(name,2,0)`), mount `CURRENTGAME:<mod>` (RIM list `0x8830` / ERF list `0x8820`), remove pass-2 (`0x88d0`) |
| 3 | `0x4c468f call 0x8800` | create pass-2 `MODULES:` table (for a potential `<mod>.mod`) |
| 4 | `0x4c46b6 push 0x7db; 0x4c46c0 call 0x8bc0` | **probe `modules/<mod>.mod` (resref `<mod>`, type 2011)**; if found → `0x4c46f9 call 0x8820` mount `MODULES:<mod>` **from the .mod** |
| 5 | `0x4c4743 push 0xbba; 0x4c4753 call 0x8bc0` | else probe `<mod>_s` (type 3002 RIM); if found: new game (`edi==0`) → `0x4c47dc call 0x8830` mount **RIM `MODULES:<mod>_s`**; otherwise `0x4c4851 call 0x8830` mount `LIVE%d:MODULES\<mod>_s` (fmt `0x7458c4`) |
| 6 | `0x4c488f call 0x88d0` | remove pass-2 `MODULES:` |
| 7 | `0x4c48d6 / 0x4c48f9 call 0x8820` | mount `LIPS:<loc>_loc` (sprintf `0x7458ac`) and `LIPS:localization` (`0x745898`) |
| 8 | `0x4c494e call 0x8bc0` (type 0xbba) → `0x4c49d0 call 0x8830` | if `<mod>_a.rim` exists, mount RIM `RIMS:<mod>_a` |
| 9 | `0x4c4a4f call 0x8bc0` → `0x4c4ad1 call 0x8830` | if `<mod>_adx.rim` exists, mount RIM `RIMS:<mod>_adx` |
| 10 | `0x4c4b2c call 0x8820` | mount `HD0:players` (`0x7458b8`) |

Loader dispatch (claim #25): `FUN_00410190` switches pass: 1→`FUN_0040fb80` (KEY, `0x4101df`),
2→`FUN_0040f200`, 3→`FUN_0040f3c0` (ERF, `0x41021d` — the live `loader_erf` hook),
4→`FUN_0040f990` (RIM, `0x41023c` — the live `loader_rim` hook); `FUN_00406e20` creates the
0x34-byte table object and calls it at `0x406fbc`. `FUN_00408830` mounts from the pass-4 RIM
list (`+0x1c`); `FUN_00408bc0` = find (nonzero eax = exists).

**Answer:** on a fresh New Game load with no `modules/<mod>.mod`, the **first module RIM opened
is `<mod>_s.rim` as `MODULES:<mod>_s`** (step 5); a `modules/<mod>.mod` would instead be mounted
as `MODULES:<mod>` (step 4) and suppress the `_s` mount; `_a`/`_adx` RIMs open only if present,
after the LIPS ERFs (steps 8–9). Retail install (read-only `ls`): `modules/` = 234 `.rim`
(117 `<mod>.rim` + 117 `<mod>_s.rim` pairs, e.g. `end_m01aa.rim` holds the 3 area files
IFO/ARE/GIT-type 2012, `end_m01aa_s.rim` the 368 static entries), **zero `.mod`, zero
`_a`/`_adx`** — so only the `_s` probe can hit. Consistent with the live loaded-save run
(docs/recon/22 §2a: `tar_m02af`/2011 probe ret 0, `loader_rim MODULES:tar_m02af_s`,
`loader_erf CURRENTGAME:tar_m02af`). The fresh-path intra-order is therefore static-closed;
the only live-only residue is observing `end_m01aa_s` actually firing, blocked by the
chargen-input blocker, not by any binary ambiguity. Claims #23, #24, #25.

## B. Dialogue-start camera path — VERIFIED statically (construction-zero holds)

* Camera creation on dialogue start: `FUN_0062bf30` (`[62bf30,62bf6d]`) —
  `if (*(param_1+0xb20)==0 && FUN_005ed530()!=0) FUN_006412f0(*(param_1+0xc4));`
  disasm `0x22bf4e mov ecx,[esi+0xc4]; 0x22bf54 push ecx; 0x22bf57 call 0x2412f0`;
  callers `0x6338aa/0x63390c/0x6342ef/0x62eae7` (dialogue-controller methods). The other two
  creation reads are in `FUN_00631d80` (`0x6320c9`, `0x632127`).
* DLG camera-model/clip path: `FUN_005a13d0` (DLG node processor; the sibling
  `FUN_005a2ae0` calls the DLG camera-field parser `FUN_0059eaa0` at `0x5a2ef9/0x5a3162`)
  calls `FUN_00631d80` at `0x5a1b98`; `FUN_00631d80` calls `FUN_0062bf70` with the DLG
  `CameraAnimation` value at `0x6320a1` (`0x23209f mov ecx,esi; 0x2320a1 call 0x22bf70`);
  `FUN_0062bf70` (`999 < val < 1728`, `+0xb28`) calls `FUN_00641010` at `0x62c00f`;
  `FUN_00641010` attaches the **`camerahook`** camera model via vtable `+0x74`
  (`0x241174 push 0x74f42c; 0x24117c call [edx+0x74]`; string rva `0x34f42c`) and maps the
  value to the clip with `FUN_006288f0` at `0x64119b` (`case 1000 -> "cut001"`, `0x3e9 ->
  "cut002"`, …, `0x2772` special — the `cut{N:03d}`/cutW/cutL/cutWL bands of docs/recon/12),
  then plays it via vtable `+0x1c/+0x18`. Claims #26, #27.
* `+0x38` on this path: all three dialogue creation sites feed controller `+0xc4` into
  `FUN_006412f0`, which forwards it into ctor `FUN_006bb670`
  (`0x64135f–0x64136f`, call at `0x64136f`); the ctor writes `+0x38` only at `0x6bb6f8`
  (`0x6bb6f1 mov ecx,[esp+0x24]; 0x6bb6f8 mov [esi+0x38],ecx`); controller `+0xc4` is zeroed
  at ctor (`0x630244`, `xor ebx,ebx` at `0x62feed`) and the `writes offset=0xc4` scan finds no
  other writer in the dialogue region (57 sites program-wide; only `0x630244` and `0x63e695`,
  both ctors storing 0). ⇒ **the dialogue-start camera object is also constructed with
  `+0x38 = 0`**; mode-nonzero branches remain unreachable. Claim #28 (cross-refs #18–#20).
  Live run b already observed this chain firing at module-load camera setup
  (`camera_mode_read 0x6320c9 c4=0` → `camera_mode_write 0x6bb6f8 value=0`); the only
  remaining live-only piece is observing it inside an actual conversation, which stays with
  lane s1-06 (input blocker), but the binary leaves no room for a nonzero mode.

## C. GFF writer layout — VERIFIED statically (save byte parity)

The save-sidecar byte-parity residual was closed by decompiling the shipped GFF writer
(same build, agent `s1_finish`, read-only on the sealed master):

* **Field append / fidx growth** — `FUN_00411730` (`[411730,411870]`) appends the field row,
  and for a multi-field struct calls `FUN_00411020` with `(old_off, old_size, old_size+4)`.
  `FUN_00411020` (`[411020,4110ef]`) reuses the old array in place when
  `fidx_len == old_off + old_size`; otherwise it copies the grown array to the buffer end and
  leaves the old copy orphaned. This is exactly PARTYTABLE's 56 extra slots (224 B, final root
  array at slot 397) and GLOBALVARS' root-row split.
* **Section serialization** — `FUN_004113d0` (`[4113d0,41153e]`) computes the six section
  offsets from the 0x38-byte header (struct 0x38, field, label, field_data, field_index,
  list_index); `FUN_00413030` (`[413030,4130fb]`) opens/writes the file and calls it;
  `FUN_00412db0` (`[412db0,412e68]`) performs append-time compaction.
* **Result** — `gff_bytes_retail` preserves the parsed struct slots/global field-row order and
  fidx table while re-encoding values. The from-scratch rewrite is 5/5 files + nested 1/1
  byte-identical on the quicksave and 100% GFF parity on every real retail slot. Claims #29–#31.

## Closure evidence index (build 0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6)

| claim | evidence_id | item |
|---:|---|---|
| #23 | ev_fd962aa1a4886068e0b9ffc5c6d26d111103a7fb04afaec1b3b55ff930f3ffc5 | A: entry chain 0x4ae860→0x4babb0→0x4c5720→0x4c44d0 |
| #24 | ev_97f328390d16d2323a7e726ca8c61d69ecb1b41b1da32a6c4fb9c3515eb93472 | A: mount order in FUN_004c44d0 (.mod probe → _s → LIPS → _a/_adx → players) |
| #25 | ev_1769fe3e3088d47f4e6995172cffbe425b58ea20d5f4b64a03357e9f9081bf71 | A: loader dispatch FUN_00410190 passes 1–4 |
| #26 | ev_618dec026831a095531fde1d6b74faaf2ecb1afbbb1b502c42d7b5987197b9cf | B: FUN_0062bf30 dialogue camera creation from +0xc4 |
| #27 | ev_16304624dda5cbaa6121ea534498ecebef8d2b0064830201b4fd0cae61211dbd | B: CameraAnimation → camerahook model + cutNNN clip (0x62bf70→0x641010→0x6288f0) |
| #28 | ev_bb7dea8a9e8b5d786d6ab34fcee6b0e2ca99a4e23f9cf966a2ed5eb37a5bd519 | B: +0x38 construction-zero on the dialogue path |
| #29 | claim 46 | C: FUN_00411730 field append + FUN_00411020 in-place/orphan fidx growth |
| #30 | claim 44 | C: FUN_004113d0 six-section layout + FUN_00413030/00412db0 write/compact |
| #31 | claim 45 | C: gff_bytes_retail 5/5 + nested 1/1 byte-identical; 100% GFF parity on retail slots |
