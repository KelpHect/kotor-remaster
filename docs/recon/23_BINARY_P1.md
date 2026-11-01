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

* Live in-process observation of the dialogue camera object's `+0x38` in a dialogue/cutscene scene
  and the synthetic multi-group mesh load → lane s1-06 (runtime attach/spawn is s1-06 only).
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
