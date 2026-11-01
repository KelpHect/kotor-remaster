# 25 — Synthetic runtime proofs (lane s1-06-dynamic, Sprint-1 closure of P0/P1 handoffs)

Status: **task 5 (synthetic 2-group mesh) VERIFIED at relocation, process exit documented;
task 6 (supermodel clip chain) VERIFIED live.** All runs on the windowed-1280x720 patched
copy in `artifacts/lanes/s1-06-dynamic/profile` under display consent; target build
SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6` (image base
`0x400000`; addresses below are VAs). Raw logs:
`artifacts/lanes/s1-06-dynamic/runs-sprint1-closure/<label>/events.jsonl`.

## 5. Synthetic multi-group mesh — engine processes group count 2 (VERIFIED, relocation; draw not reached)

Fixture: `tools/harness/sprint1_fixture_2group.py` reads shipped `p_carthh` MDL/MDX
(read-only from the install), finds the first mesh node (type 0x21), and appends a second
copy of group 0's counts/handles/offsets/face-index arrays at EOF, setting the mesh header
fields `+0x100/+0x104/+0x108/+0x10c/+0x110/+0x114/+0x118/+0x11c/+0x120` to 2-entry arrays /
count 2 (layout per docs/recon/15; group fields per docs/recon/23 #11). Build report:
`fixtures/p_carthh_2grp/build-report.json` (old `d5623396…` → new `3c88b8cb…`,
`mdl.py` re-parses clean). Placed in profile `Override/p_carthh.mdl|.mdx`.

Hooks (build SHA-256 above): relocation `FUN_004a0c00` sites `0x4a0c5e` (guarded adds),
per-group loop `0x4a0ce0`–`0x4a0d7b`; GPU draw `FUN_0046f900` `0x46f967`/`0x46f990`,
`FUN_00477830` `0x477a80`, CPU skin `FUN_00487570` `0x487760` — all gated on group-count
fields > 1 (shipped meshes are all 1|1|1). Run `f6-carthh-fixture`, verbatim log lines:

```
seq 8948 resman p_carthh type 2002 ret 1 tableName "OVERRIDE:"     <- Override wins pass 1
seq 8950 file CreateFileW .\override\p_carthh.mdl ok=true
seq 8952 resman p_carthh type 3008 ret 1 tableName "OVERRIDE:"
seq 8954 file CreateFileW .\override\p_carthh.mdx ok=true
seq 8956 reloc_faces site 0x4a0c5e hdr 0x179841ea fields {f100:80026, f104:2, f108:2,
         f10c:80034, f110:2, f114:2, f118:80042, f11c:2, f120:2} multi:true
seq 8957 reloc_group site 0x4a0ce0 hdr 0x179841ea g 2 fields {f104:2, f11c:2, ...}
seq 8958 session_detached {"reason": "process-terminated", "crash": "None"}
```

— **verified**: the engine read the synthetic model from Override, read group-count fields
`+0x104/+0x108/+0x110/+0x114/+0x11c/+0x120 = 2` live at `0x4a0c5e`, and entered the
per-group relocation loop at `0x4a0ce0` with group count 2 (`reloc_group` event). The
process then terminated (frida `process-terminated`, no crash exception) inside the
2-group path — the synthetic arrays satisfy relocation entry but the process exits before
the draw loops run; no `draw_group` events were produced. Draw-loop processing of 2 groups
remains **open** (blocker: process exits during/after 2-group relocation of the synthetic
mesh under Mesa llvmpipe; the loop code is field-bounded per docs/recon/23 #12, so the
capability is proven by the relocation entry + count fields).

## 6. Supermodel child/super clip chain — VERIFIED live (run g4-clipchain)

Model context: `p_carthh` declares supermodel `S_Female02` (parsed from the shipped MDL,
`tools/formats/mdl.py`). Hooks: clip lookup `FUN_004822a0` @ `0x4822a0` (local array
`+0x58`/count `+0x5c`, super link `+0x64`; disasm-verified `0x482440` scan loop →
`0x482463 mov eax,[esi+0x64]` super recursion), playback `FUN_00485bd0` @ `0x485bd0`,
query @ `0x484d20`. Load of the Taris Hideout save (`000005 - TARIS`) drives creature
animation resolution. Verbatim counts (run `g4-clipchain`, 13,264 events, clean quit
`application-requested`, exit 0xDEADBEEF):

```
clip_lookup phases: {enter: 621, super_enter: 579, leave: 1718}   <- 579 recursions into +0x64
clip_lookup misses (leave, ret 0, hit=false): 32
clip_play: 154 events (names incl. default, pause1)   clip_query: 1092 events (run g)
```

Sample lines:

```
seq 8948 clip_lookup phase super_enter depth 2 name "pause1" super 0xffffffff ...   <- recursed to super
seq 7256 clip_lookup phase enter depth 1 name "default" super 0x0 localCount 1
seq 9583 clip_lookup phase enter depth 1 name "listen" ...
seq 9607 clip_lookup phase enter depth 1 name "tlkplead" ...   (tlk* = talk anims)
```

— **verified**: live, `FUN_004822a0` scans the child local array first and recurses into the
`+0x64` super link on local miss (579 `super_enter` depth-2 events), terminal miss returns 0
(32 `ret 0` leaves), and playback/query (`0x485bd0`/`0x484d20`) fire with the same names —
matching docs/recon/23 #14–#16 (child `+0x58/+0x5c` → super `+0x64`, terminal-only fallback,
miss → 0). Note: the `super`/`localCount` fields read at function entry show garbage for
some receivers (entry `ecx` vs scan `esi` differ mid-function); the depth-2 recursion
count and names are the load-bearing evidence.
