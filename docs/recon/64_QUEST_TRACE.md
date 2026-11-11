# Quest trace: static graph, per-edge execution, navigation reachability

Status: **gates in CTest**: `kotor_quest_trace`, `kotor_conversation_flow`, `kotor_plot_replay`, `kotor_vm_conformance`,
each with a `_negative` twin.
Sources: `engine/src/game/QuestGraph.{h,cpp}`, `QuestTrace.cpp`, `QuestTraceOps.h`, `QuestFlow.cpp`,
`engine/tests/vm_conformance.cpp`. Baselines: `engine/tests/fixtures/quest_trace_baseline.tsv`,
`conversation_flow_baseline.tsv`, `plot_replay_baseline.tsv`, holding tags, resrefs, numeric IDs and counts only.

## 1. What this proves and what it does not

It proves that, for each edge marked pass, the real DLG condition and script, run through the
real `GameSession`, VM and `ScriptHost`, produce the journal, global, XP, item or module effect
the shipped data says they should, once the edge's preconditions are in place.

It **does not prove** that:

- a player can win the fights;
- timing and cutscene sequencing work across real frames;
- a run keeps state over hours;
- a player can reach an edge's preconditions in play. They are written by the harness;
- conversations flow correctly from node to node. Each node runs alone here; section 9 walks them.

**Harness state writes are allowed in this mode only because it proves script and engine
correctness, not play.** A pass here is not a playthrough result and must not be reported as one.

## 2. Method

**Stage 1, static graph** (no simulation). Reads all modules and global resources from the install,
read-only. A symbolic NCS walker forks on `JZ`/`JNZ` and turns guards into requirement sets. Each
edge records who sets an effect, what gates it and what later reads it. Action names are checked
against `nwscript.nss`. `ExecuteScript` targets are followed, including the object each one runs on,
because that object is the called script's `OBJECT_SELF`.

**Stage 2, execution** (no walking, no combat). For each setter edge, per module, one process at a time:
restore a pristine snapshot, write the recorded preconditions, run the host, assert the effect.

- *Answered queries.* An action named by a gate that state writes cannot satisfy, such as
  `GetIsObjectValid` or `Random`, is answered by the harness, up to 64 calls per run, then the real
  handler runs. A pass that relied on this is counted as `answered`, and is weaker evidence.
  A missing effect under an answer is can't-determine, never fail. The harness tries a real sequence of
  answers before giving up (`sweepAnswered`), and for a choice among global values the shipped writers allow it
  tries them all (`sweepInfeasible`).
- *Dead edges.* `dead` means shipped code or data shows no run reaches the effect; it is never a pass. Accepted proofs,
  each derived from shipped data, not from a list of names:
  `unreachable_owner_never_placed` (no GIT, no constant `CreateObject`, in any module, and no fragment that could build
  the name), `unreachable_no_caller` (nothing ships that calls the script), `run_var` (a whole walk with `GetRunScriptVar`
  pinned to the caller's constant never reaches the call), `owner_tag_differs` (the script branches on its own tag and every
  holder of the conversation has another), `global_undeclared` and `global_wrong_kind` (retail drops the write, 00529030),
  `template_not_shipped` and `template_only_in` (005608b0 fails, no item), `no_route_in_dialogue` (no link reaches the node),
  `link_vs_script` (the link's condition and the node script need opposite values of one global, and retail reads the
  condition's wrong-kind global as 0), `waypoint_not_placed` (`GetIsObjectValid(GetWaypointByTag(x))` in a module that places no
  waypoint x and where nothing creates one; where a walk stopped early the site must also lie in code entered only through
  that test, which is read from the code), and `boolean_read_vs_value` (`GetGlobalBoolean` compared with a value it cannot
  answer: `k_pebn_korrib`, `k_pebn_manaan`, `k_pebn_kashy`, `k_pebn_tatoo` test `K_CAPTURED_LEV` == 5). A sweep that finds no
  working value proves nothing: other variables stay as solved, so the old `infeasible_swept` is gone.
- *Directed search* (`sweepDirected`). For what is still open, four phases over the same seeds: earliest query first, latest
  query first, then both with the host as the controlled character (`GetFirstPC` answers with self), and in those two the
  globals the path wants are written as first read. Queries with state behind them write the state and let the real handler
  read it. `GetTag` of self is never answered: its value is the owner's real tag. `GetWaypointByTag` is never answered either: a
  waypoint is placed or it is not. A pass that used any answer is counted as `answered`.
- *Thorough walk.* Where the site's script asks `Random`, a die, `IsNPCPartyMember` or `GetHitDice`, a fifth phase walks those
  values breadth first: every single changed answer, then every pair, and so on, once per distinct way in. It is once per
  script and has a whole-run limit. A plan that worked is tried first for the same site in the next module. `k_plc_tres*`
  and the `K_PARTY_STORE` pairs settle here.
- *Budget.* The search counts work (VM instructions plus a fixed cost per run), never time, so two runs agree edge for edge. The
  limits were set by running, not derived. `deadlineSeconds` is a safety stop; reaching it fails the gate.
- *A pass needs its site.* An effect only counts if the site's own instruction ran in that run. The same global written by
  another block of the script is `effect_from_other_site`.
- *Dialogue conditions as hosts* (`dlg_cond`). A write in a condition script counts as an edge whose host is that condition.
- *GUI exit edges.* `ShowPartySelectionGUI` and `PlayPazaak` name exit scripts, which run after the panel; they are edges.
- *Sentinels and observed writes.* A global with an unknown expected value is preset to a sentinel, and
  writes are observed, so a write that a later one overwrites still counts if the store kept the last.
- *Node isolation.* A dialogue node runs through `applyDialogueNode` without walking on.

**Stage 3, navigation** (no frame stepping). From each arrival (entry point or linked door), walk the
closed walkmesh, within 4 m of the object. A door's state and how it opens (script, key, skill or lock
flag) comes from the graph and the placed data. Subjects with no placed object are `not_placed`.

Output files in the `--quest-trace` directory: `quest_graph.json`, `quest_trace.tsv`, `quest_nav.tsv`,
`quest_fail.log`.

## 3. Results

Graph: 117 modules, 101 quests, 643 entries, 15408 edges, 8666 scripts (925 walks truncated, 0 decode failures), 1167
dialogues, 70791 nodes. All 772 action names match `nwscript.nss`. Edges and nodes rose because companions and messengers
are now hosts: a `create_object` of a global creature template, and the nine party templates on `ebo_m12aa`. Flagged: 111
unresolved dynamic arguments, 22 entries with no setter, 58 globals read but never set, 69 globals spelled in more than one case,
36 not declared in `globalcat.2da`.

Edges, this binary against the start of this work:

| Class | Before | After | Why |
|---|---:|---:|---|
| pass | 7840 | 8409 | 6640 real, 1769 answered (1084 before) |
| fail | 0 | 0 | |
| can't determine | 4173 | 270 | |
| dead | - | 6729 | a new class, never counted as a pass |
| `opaque` | 1642 | 172 | see below |
| `effect_missing_mock` | 817 | 41 | |
| `condition_false_opaque` | 298 | 24 | |
| `owner_not_available` | 146 | 12 | |
| `infeasible` | 1159 | 0 | |
| `condition_vs_branch` | 50 | 0 | |
| `orphan_no_context` | 10 | 1 | `k_ptat_chiefitem`: no caller found |
| others | 51 | 20 | `effect_from_other_site` 8, `holder_cannot_hold` 6 (droid inventory at death on `sta_m45ac`), `condition_not_modelled` 6 |

Dead: `unreachable_owner_never_placed` 3540, `unreachable_no_caller` 1799, `run_var` 1123, `owner_tag_differs` 126,
`waypoint_not_placed` 52, `global_undeclared` 35, `boolean_read_vs_value` 22, `global_wrong_kind` 9, `no_route_in_dialogue` 8,
`template_not_shipped` 7, `link_vs_script` 7, `template_only_in` 1. An independent audit re-derived these and refuted five edges; see
section 12.

Earlier passes that did not hold: 20 became dead, because the harness had answered `GetWaypointByTag` or `GetIsObjectValid` for a
waypoint the module does not place (`k_pdan_rapid03`, `k_pman_rapid03`, `k_pman_rapid04`). 51 became can't-determine, because the effect
they were credited with came from another line of the script, or because they rested on an answered query that no longer holds.

`k_ai_master`: once `SetGlobalBoolean` of `END_HURT_WARN`, reached through the shipped default AI hooks. 1123 are `run_var`,
the others are on owners never placed or scripts with no caller, and the `k_hen_damage01` ones pass: the host is the party leader,
`END_BRIDGE_COMBAT` is written set and the warning unset as the path reads them.

`k_pebn_swoop`: `dead`, `unreachable_owner_never_placed` of `pebn_swoopdrd`. The name is never placed in any GIT or created by
any script, and no script builds it. Its old passes showed the script's behaviour only, so they no longer count as reachable.

The largest groups still open are in `quest_trace.tsv`, each with the query it stopped at: the mini-game `heartbeat` scripts 29, whose
timers and gears read `GetTimeHour` and the race state; star map scripts `k_pla_actmap`, `k_pman_starmap06`, `k_pkas_starmap`, `k_pkas_openmap`,
`k_ptat_starcut` 34, whose `K_STAR_MAP` value depends on the module and the map's order (the replay runs these on real state); `k_pman_26b_area`
10; `k_pebn_basttalk` 9; `k_sup_gohawk` 9; `k_act_makeitem` 9; the ship game's `k_pebo_sthdeath*` 24; and the Pazaak exit scripts 17, which read
`GetLastPazaakResult`.

Per planet, as pass / can't / dead:

| Planet | Pass | Can't | Dead |
|---|---:|---:|---:|
| taris | 2351 | 26 | 1258 |
| manaan | 1079 | 40 | 244 |
| ebon_hawk | 776 | 26 | 3465 |
| tatooine | 669 | 31 | 162 |
| korriban | 657 | 23 | 167 |
| cutscene | 636 | 0 | 218 |
| dantooine | 613 | 12 | 249 |
| rakata | 481 | 0 | 256 |
| kashyyyk | 357 | 37 | 137 |
| leviathan | 304 | 6 | 121 |
| star_forge | 185 | 26 | 101 |
| endar_spire | 89 | 5 | 56 |
| global | 81 | 12 | 267 |
| other | 73 | 29 | 0 |
| yavin | 54 | 1 | 28 |

Main-plot chain from `end_m01aa`: complete to both endings. This is static connectivity: shortest routes may use non-plot
shortcuts, so it shows the links exist, not that this order is the plot's. Section 9 chooses the route on live state.

Navigation: 461 reachable, 345 through doors, 5 within reach of the floor, 297 not placed, 7 unreachable, of which 2 are on the
main path (both excepted, section 4).

**Journal rows with no setter (22).** Each is a row of a quest whose other rows are written, except `tar_meetcand`, which nothing
writes at all. No shipped script or DLG `Quest` field names these values: `end_attack` 10 and 40, `k_swg_bastilatalk` 60,
`kas24_removepoachers` 20, `kor33_enteracademy` 45, `kor35_findingdustil` 25, `sta_confront` 1, 2, 20, 98, 99, `tar_escape` 15,
`tar_meetcand` 1, 99, `tat17_starmap` 10, 20, 50, `tat17aa_jawarescue` 10, `tat17ae_signingnico` 10,
`tat18ac_dragonhunt` 40, `tat20aa_worthy` 20, `unk_trapped` 25 are unused journal text. `kor35_findingdustil` is written at
10, 20, 30, 40, and 25 at no site. The other 33 of the earlier list of 55 were false: they were DLG `Quest` fields in companion
and messenger conversations with no host then.

**Globals read but never set (58).** One is the empty name, an unresolved argument. The rest fall into: names `globalcat.2da` does
not declare, so retail reads 0 whatever a script does; and names it declares that no shipped script writes, so the read is 0 for the
whole game (the `MAN_*_DONE` and `DAN_*_DONE` families, `MAN_DAKOTA_STATE`, `MAN_JENNA_STATE`, `Tar_PazLow`). Reads of 0 here are shipped
behaviour.

## 4. The gate

`kotor_quest_trace` fails on a new engine-side edge failure, a new unreachable main-path object, or a
pass count under `pass_floor`. Allowed exceptions, each with its reason in the baseline:

- `korr_m35aa kor35_sithduel`: a walled 35-triangle pocket, 4 m past reach. `k35_init_duel` changes its faction and `k35_duc_fight`,
  `k35_ud_captive5`, `k_pkor_duelend` and `kor35_enter` address it by tag, so the fight comes by script. Whether it can reach the
  party is not proved.
- `lev_m40ac lev40_hangartalk`: an invisible speaker on a 3-triangle pocket, 31 m from the floor. `k_ptar_jumpebon` starts its
  conversation with `GetObjectByTag` and `AssignCommand` under a fade: nothing walks to it.

No edge exceptions.

`kotor_quest_trace_negative` runs `manm27aa` with `--quest-trace-break journal`, which turns action 367 into a
no-op, and requires the gate to fail. It uses the same baseline, and that matters: without a baseline any
known failure would satisfy its expected output even with no break.

Full run: 14 to 16 minutes, about 860 MB peak (it was 6.5 GB: a search queue holding trials no phase could reach, on
`k_plc_tresciv`; capped).

`pass_floor` is 8409, the exact count. Two full runs before the last search change agreed on every row, status and reason; that change moved only 4 edges. It only moves up. A code change that legitimately
moves an edge changes this number, and when an earlier pass is shown false it has to come down, with the reason written here.

## 5. Engine defects found by this mode

1. Global names are case-insensitive: `World.h`. Retail setters 00529570 and 00529680 and the matching read
   00529240 go through 00529030, which hashes the lower-cased name (00528e50, 006fd268) and compares with
   `__stricmp` (005e58c0). `k_punk_redvill99` writes one global in two spellings.
2. `removeJournalEntry` is case-insensitive, like `journalState` (005e58c0). That `ACT_368` itself compares this
   way is inferred, not fully traced.
3. `nextInShape`: `ACT_128`/`ACT_129` share one area cursor over objects sorted by x (0054a260, 00506f20,
   0054ab7d). Nested iteration in `k_ai_master` never ended. Shape 0's range is simplified to a box around its ends.
4. `templateFor` kept a process-wide list of missing templates, but templates ship per module.
5. VM `JSR` limit was 8; retail has a 128-entry return stack (005d17f0, called at 005d3f17). The 8 at 005d226c limits
   nested script runs.
6. Walkmesh seams: room edges with differently subdivided sides were not linked, leaving the star map on
   `korr_m39aa` unreachable. Perimeter edge records from the WOK now mark room transitions, and such edges link across rooms.
7. `ShipBuild` (761) had no handler and returned 0; retail 00543840 pushes 1 always, so debug branches ran.
8. Found by the plot replay: `Unescapable` belongs to the area (+0x2ac, read from the ARE by 00508c50, written by 005424e0,
   saved by 00506090), and the engine kept one session-wide flag and never read the ARE. It also refused doors and walk-in
   transitions while it was set. Retail reads the field only in 005367b0 and two GUI panels, 0062dd20 and 00693650, so no door
   looks at it; a scan of every `+0x2ac` operand in the binary found no other reader on an area. The door rule is gone and
   the flag is per area. What those panels do with it is not fully read; they look like the save screen, which the engine
   does not gate on it. Not changed.

## 6. VM oracle change

The functional oracle `artifacts/lanes/s6-02-actions/functional_oracle_s6.json` came from a Python runtime with
case-sensitive globals, so it expected 3 globals for `k_punk_redvill99__b82694ee02f65947.ncs` where retail has 2
(defect 1). `tools/vm/state.py` now folds case. Regenerating with `tools/vm/corpus_run.py --runtime` and comparing all
fields of all 8666 files changed that one file and nothing else, with the JSR limit of 128 in place.

The Python runtime stays an independent implementation, so the oracle still means what it did. `artifacts/` is
git-ignored, so another checkout has to regenerate its oracle with the same command.

## 7. Script questions settled

**`OBJECT_SELF` in dialogue** is the conversation owner for conditionals (0059ec90), node scripts (0059ed70) and end
scripts (005a0a40, called from 005a3640, 005a3820, 005a4010, 005a03c0). Speaker only chooses who animates and sounds.
An empty condition is true; a failed or void one is false.

**Speaker** (0059fc30): cached per conversation (+0x84). The owner's own tag gives the owner, otherwise the nearest object
of that tag in the area within 1000 (`DAT_0074744c`), otherwise nothing. A link with an empty speaker or the owner's tag
passes. Any other link whose speaker does not resolve is skipped **before** its condition runs (005a3640, 005a03c0).
`OWNER` and `PLAYER` are special only in 005a3820, for presentation. The engine now does this (`dialogueSpeakerResolves`).

**Globals**: retail looks a name up in `globalcat.2da` (00529030). A name that is undeclared or of the wrong kind drops the
write and reads 0. Numbers are one byte, sign-extended (00538dc0, 00542b60). The engine now does this. The catalog has 1185
rows (809 boolean, 369 number, 5 location, 2 string). Shipped scan: no constant `SetGlobalNumber` outside -128..127 (the
largest is 101), though non-constant writes exist; 38 undeclared names at 53 script sites; 37 wrong-kind names at 53 sites.
It matters: for example `dan_bolook_anger` and `k_captured_lev` are numbers that shipped scripts also read with
`GetGlobalBoolean`, which retail answers with 0. 58 edges rest on this and are `dead`. Tests that invent names now declare
them with `declareGlobalForTest`.

**`ExecuteScript`** (00535b70): pops name and object, the optional variable stored to `DAT_00832828` with no compare (00535c13, so -1 is stored too), nested
synchronous run with self = target; an invalid target runs with invalid self. Requirements on `OBJECT_SELF` now carry through the target.

## 8. VM conformance

`kotor_vm_conformance` runs 135 small NCS programs with expected results cited by address;
`kotor_vm_conformance_negative` perturbs the expectations and must fail. The retail interpreter is 005d2bd0.

| Area | Retail | Was |
|---|---|---|
| Integers | 32-bit wrap; shift counts `b & 31`; `SHRIGHT` of a negative rounds toward zero; `USHRIGHT` is arithmetic (005d36da) | wider, different |
| Floats | single precision | double in places |
| Divide or modulo by zero | stops the run, -0x69 | gave 0 |
| Type check on pop | 005d24c0, 005d2510, 005d25e0, 005d26a0: -100 | looser |
| Bad qualifier, unknown opcode | -0x61, -0x65 | looser |
| Instruction limit | `++counter > 0x1ffff`: -0x5d, reset only at the outermost run, shared across `ExecuteScript` | 200,000 per run |
| Return stack | 128 entries, shared across nesting: -99 | 128 per run |
| Nested runs | 8 (005d2260, 005d23e0) | 32 |
| On error | 005d4270 reports, restores SP and return stack; side effects stay | |
| Return value | 005d45d0: success with exactly one int cell returns it; none, no value; else 0 | |
| Equality | by the right cell's tag; floats, ints and objects by bits; strings ASCII case-insensitive (005e6450); engine types by vtable 0052c510 | |
| Empty string | normalized (005e5a90) | |
| `MOVSP` positive | no-op (005d21f0) | |
| `INC`/`DEC` | q==3 and an int only (005d16c0) | |
| `CONST` object | 0 is self when valid, anything else invalid | any id. Fixtures use `keepObjectIdsForTest`, which shipped paths never do |
| `STORESTATE` | resume at ip + qualifier, +2 base bytes, +6 stack bytes | |
| Action arguments | handlers pop what they declare; extras stay on the stack | |

Nesting: 8 stands, since no hook or dialogue script is nested in retail (each is an outermost run with a fresh counter), and
static `ExecuteScript` chains in shipped scripts go at most 3 deep, so 8 never binds on constant names.

Argument counts: 636 shipped sites pass a count other than declared. `AddPartyMember` with 1 of 2 in `k_act_carthjoin` and
`k_ptar_missjoin`, and `ActionBarkString` in `k_plev_corpse1`, make the handler's typed pop fail, so retail stops those runs
there too. That is shipped behaviour, kept.

`kotor_vm_corpus` now separates `retail_run_errors` (divide by zero, typed pop) from `vm_errors`. The former are held to the
oracle file by file; any other VM fault still fails the run. Under the corpus's typed-default mock 33 scripts divide by a
mocked zero and 3 hit the type check.

Not determined: a NaN compare (x87 `LEQ`); compare fields for effect and event types; `INT_MIN / -1` (an `idiv` trap in the
handler); the stale return at +0x1c in a void conditional after a nested run; negative `MOVSP` and unchecked upper bounds;
handler-specific uses of the argument count; retail decodes one instruction at a time while the engine decoder rejects a
whole file on an unknown opcode; how far the return stack limit interacts with nesting; the link flag at +0x14 that lets
005a3820 append failed replies to the list.

## 9. Sequence proofs

Both use real `GameSession` state. Neither runs frames of play except for transitions.

**`kotor_conversation_flow`** loads each of the 47 main-path modules and walks each dialogue node to node, choosing only replies whose real
conditions passed, preferring nodes not yet seen, up to 12 paths each. On each path it checks VM problems, refused scripts and the
unconditional journal and global effects of nodes it visited. A journal entry that a later node on the same path removes is not checked
(`k_pebo_sasha01` does this). Three passes, counted apart:

1. Blank state: what a new module shows.
2. Plot states: a replay in the same process supplies each module's first and last state and three from across the plot.
3. Condition state: each link still shut is tried with its own condition's state written true, up to 24 rounds per dialogue. This shows
   that the nodes behind it work, not that the plot can make that state.

Result: 397 dialogues, 25478 nodes. Blank state 6693, plot states add 617 (7310, 29%), condition state adds 7193 (14503, 57%; not a reachability figure), 14 blocked at start. Plot states add little because the replay only runs master counters. The negative check drops global writes on two modules and must fail. No exceptions.

**`kotor_plot_replay`** runs one session from `end_m01aa` to both endings. At each stop in `Graph::checkpoints` it runs the plot's master
counters (`K_KOTOR_MASTER`, `K_STAR_MAP`, `K_CAPTURED_LEV`) by the shipped edges whose conditions can be read, and it saves and reloads,
comparing globals, journal, module, XP, gold and party. Between stops the route is chosen on live state among shipped transitions, trying
those whose conditions hold; when every hop of the best length is shut, it looks again without them for a longer shipped way round.
Scripts with no known caller are not routes. The final choice forks at `unk_m44ac`, and the two endings run from one save.

What it shows on real state: the Taris exit gate in `m12ab`, the galaxy map to planet route, each star map adding to `K_STAR_MAP`, the
capture edge passing on `K_CAPTURED_LEV` written by a star map script, the Manaan route `manm28aa` to `manm28ab` to `manm28ac` to `manm28ad`,
and the Taris doors `tar02_lowdoor` and `tar03_javydoor`, which their own `OnFailToOpen` scripts open.

Not played, listed and allowed: `no_chargen`, `no_walking`, `kill`, `answered`, `door_opened`, `minigame`, `party_panel`. Everything else
fails the gate unless a row names it: `leg_forced`, `node_forced`, `spawned`, `pc_for_owner`, `item_given`, `script_direct`. Those rows, in
`plot_replay_baseline.tsv`, are **steps not proved**.

No leg is forced. `danm14aa` to `danm15` crosses `man14aa_door04` once `k_pdan_14a_area` has unlocked it on `DAN_RUINS_GONE`. For a door still shut
the replay looks for what unlocks it and goes to get it, by shipped routes only: `k_pdan_vandar04` needs `DAN_JEDI_PLOT` 7, which climbs through
the values its own edges write (1, 2, 3, 4), including the trip to `danm14ac` where Juhani's conversation writes `DAN_JUHANI_PLOT` 3. On such a
trip a one-shot start is talked through, a reply that led nowhere is not taken again, and `k_pdan_saber15` holds until `dan13_dorak` clears it.
This is not the order a player must follow, only an order that real conditions allow. A trip never forces a leg.

The forced nodes are all one of: a start link an earlier walk used up (`k_con_talkedto`), so the replay's own order; a counter the replay ran
before the side conversation that leads to it; or a side choice not played (romance, Light or Dark, `k_punk_pcgood`, `k_sta_maldead`,
`k_psta_carthrom5`). None looks like a shipped plot order bug. No row matches a documented shipped bug. The community records were used only
as hypotheses.

Neither mode proves: that a player can win a fight or finish a side quest, timing, cutscene order, AI, or any step in the
baseline.

## 10. Known limits

- 270 can't-determine edges remain, all named in `quest_trace.tsv` with the query they stopped at.
- 925 walks are truncated. A write the walk never reached is still found by a flat scan, with an unknown gate. That now also covers walks
  that stopped following a state after a few visits (`UNK_TILES`, the floor panels of `unk_m44ab`).
- `man28_airlock` R6 at `k_pman_airlock11@3737` passes as `answered`, which shows the script's branch, not the holder: in play the holder with tag
  `28d` is reached only behind `MAN_SHORTCUT` (`k_pman_lock01`, written by `k_pman_at02` from the far side).
- 1769 passes rest on an answered query, among them answered `GetIsObjectValid` for objects a run did not make. They show the script's behaviour on
  that answer, not that the world gives it.
- Community records were tested as hypotheses; none is relied on as evidence here.

Two harness choices since the Sprint 14 count. A module's load now runs what OnEnter deferred before the
pristine snapshot: `k_pebn_pophawk` destroys the placed Carth at 0.5 s and respawns him at 2 s, and left pending that
fired inside the first run and dropped the closure a reply had just queued on the old Carth. And a host that calls
`PlayPazaak` now runs its end script as `resolvePazaakIfOver` would, since no match is played here;
`GetLastPazaakResult` is answered by what the branch needs, so those edges pass as `mock:`.

`GetObjectByTag` ignores case (`005e6080` lowers the tag), so nine `k_punk_floor0N` edges that passed because
`FloorPanel0N` found nothing now need all nine panels' local booleans, which the harness can't set: pass to cant.

## 11. Doors and tags

**A door opening itself passes its own lock.** `k_ptar_alertsith` and `k_ptar_alertboun`, the `OnFailToOpen` scripts of the Taris lift and of
Javyar's door, open a locked plot door with `ActionOpenDoor` on self and nothing unlocks it. Retail 0058bbb3 fires the script and at 0058bc03
looks in the door's own queue for action 0x14 aimed at itself, and then skips the locked feedback. The engine ran `OnFailToOpen` again instead,
so neither door could open in play. Fixed in `ScriptActions.cpp`. A door standing open can then be crossed (`ScriptWorldState.cpp`); that open
passes the lock on the click is inferred from those scripts, since the retail click path was not traced. Neither is the action 0x14 handler itself.

**A GIT instance's tag is not read.** Retail takes the tag from the template and only link fields from the GIT: triggers 0050a37b (template 0050a473,
tag in 0058dfa1), doors 0058c5f0 (template 0058c641, tag in 0058a5a7). The engine's `Entities.cpp` agrees. 12 instances differ from their
template. `k_pkas22ab_enter`, `k_pman26d_enter`, `k_pman_vedroid1`, `man_vek_door`, `k_plev_ambush_en` and `k_psta_actwait3` use the GIT tag, so by
this they find nothing in retail. That is from the data; it was not seen at run time.

## 12. Audit of the dead edges

An independent check re-derived each proof. Refuted and fixed:

- `k_plev_resspikes` was not without a caller. The placeable's `OnEndDialogue` holds it: retail loads the field at 005860ed, 005a0b30 calls 00585460,
  which runs it. The engine ran no such hook and now does. `OnDialog` is loaded too, but what fires it was not traced.
- `tar02_sithbasekeycard` ships. A resref holds 16 characters (00406290), so this is `tar02_sithbaseke`. Item lookup now cuts the name.
- `k_ptar_zelkarm0x` was not refused by the owner's tag: `k_ptar_sithdis` asks `GetTag` of the armour worn. The mark now ignores a link
  script's `GetTag` when that script asks about another object.
- The Tatooine droids were not infeasible. A killed creature is destroyed: 004e0ac0 posts event 11 and 004fece0 deletes it, about 3 s later when
  `appearance.2da` has no delay. The two reads of one droid come from an earlier test's short circuit.
