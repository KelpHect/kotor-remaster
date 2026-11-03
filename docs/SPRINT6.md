# Sprint 6 - P5 script runtime and systems core

Status: **complete - gate S6 met (2026-11-03)**. This is the first P5 systems sprint after the
playable P4 slice. It brings the verified script and rules model into the engine's own C++ path: a
native NCS VM, a functional action runtime bound to the live game world, real dialogue conditions,
a 2DA-driven character sheet with d20 combat rounds, and party/inventory/journal state. Python
remains a test oracle only; the runtime never calls it. The exit report is
[SPRINT6_REPORT.md](SPRINT6_REPORT.md).

Baseline: Sprint 5 / gate S5 (`docs/SPRINT5_REPORT.md`): the C++ runtime plays `end_m01aa`, the
13/13 CTest suite is green, and the scene parity diff is 0.03/255. The engine has no script
execution, dialogue conditions, party, inventory, journal, or rules tables yet.

## Gate S6

1. **Native NCS runtime** - a C++ NCS V1.0 decoder and VM implement the full `0x01-0x2D` opcode
   set, the typed 4-byte stack, BP/globals, the JSR/RETN return stack with recursion limit 8,
   `DESTRUCT`, and `STORESTATE`. All **8,666/8,666** unique shipped scripts execute with zero
   decode errors, zero VM faults, zero unknown actions and zero invalid jump targets, and match
   the current verified typed-default oracle (regenerated after the `SAVEBP` fix) **file-for-file**
   on status, steps, max stack, action counts, top-of-stack and invalid jump targets.
2. **Functional action runtime** - all **772/772** action ids are bound. The 344 ids exercised by
   the corpus have functional handlers over a deterministic world model (objects, tags, areas,
   locals, globals, party, inventory, action queue, timers, string/vector/math, journal and
   mediation). A full-corpus functional run has zero decode/VM/unknown-action faults, **0
   unclassified** ids, and matches the Sprint 3 corpus totals (303,616 ACTION calls, 472
   `ActionWait` blocks, 9 budget stops).
3. **Engine script hooks** - ARE, IFO, GIT instance and UTC/UTD/UTP/UTT template hooks are loaded
   and attached to real scene entities. Module/area enter, creature and placeable spawn, heartbeat,
   trigger enter/exit, door and placeable interaction, and death hooks execute the shipped scripts
   with the correct self/event objects. `--script-test` proves real state changes from the shipped
   `k_pend_area01`, `k_pdan_trask_9`, `k_pend_trig02` and `k_hen_dialogue01` programs.
4. **Dialogue conditions** - the DLG `StartingList.Active` condition scripts are executed and the
   first truthy entry is selected; entry/reply `Script` action scripts and `Quest` fields execute.
   The real `end_trask01` conversation has 19 conditional starting entries and must no longer fall
   back to "first line with text". `--dialogue-test` proves the selected entry and its script
   effects.
5. **Character sheet and d20 rounds** - classes, attack bonus, saves, hit dice, skill points, feats
   and equipment slots come from the shipped 2DA tables (`classes`, `cls_atk_*`, `cls_st_*`,
   `exptable`, `skills`, `feat`, `baseitems`). Combat uses 6-second rounds, initiative, d20 attack
   rolls against AC, natural-1/natural-20, critical threat/confirmation, damage dice, death and
   XP/level-up from `exptable`. `--rules-test` proves each formula and a deterministic round.
6. **Party, inventory, journal** - party roster and join/leave, inventory add/remove with base-item
   slot validation and equipping, gold, journal/quest state and XP are real engine state. Script
   actions (`GiveXPToCreature`, `AddJournalQuestEntry`, `GiveItem`, `CreateItemOnObject`,
   `AddPartyMember`, `AddJournalWorldEntry`, ...) mutate them. Save/load persists the character,
   party, inventory, journal, globals, locals and module state in the engine slot.
7. **Verification** - all previous 13 CTest targets plus the new VM, script, rules and systems
   targets pass; the Python tool suite stays green; the install is read-only; the report, recon
   note and evidence index are committed.

## Lanes

| Lane | Scope | Output |
|---|---|---|
| s6-01-vm | NCS decoder, VM, generated action table, corpus runner | `engine/src/script/`, `tools/analysis/gen_cpp_action_table.py` |
| s6-02-actions | functional world/action runtime, behaviors, timers | `engine/src/script/World.*`, `ActionRuntime.*` |
| s6-03-hooks | GFF hook loading, event args, module/area/entity execution | `engine/src/kotor/`, `engine/src/game/` |
| s6-04-dialogue | `StartingList.Active`, entry/reply scripts, quests | `engine/src/game/GameSession.*`, `engine/src/kotor/Dlg` |
| s6-05-rules | character sheet, 2DA rules, d20 rounds, XP/levels | `engine/src/game/Rules.*` |
| s6-06-systems | party, inventory/equipment, journal, save/load | `engine/src/game/`, `engine/tests/` |
| s6-07-gate | corpus parity checker, script/rules/systems gates, CTest | `engine/scripts/`, `engine/CMakeLists.txt` |
| s6-08-report | report, technical note, evidence index | `docs/SPRINT6_REPORT.md`, `docs/recon/43_SCRIPT_AND_RULES.md` |

## Non-goals (Sprint 7)

- Merchants/stores, encounters and spawning tables, minigames (pazaak, swoop, turret).
- Cutscene cameras, movies, full audio, lip sync and the full `.gui` screen set.
- Original KotOR save compatibility. Sprint 6 persists the engine save schema only.
- All 117 areas. The slice remains the Endar Spire pair plus the Taris apartment pair.
- Creature animation/skinning and weapon attachment.
- Full feat/Force-power selection UI and every spell effect. The sheet and validation exist;
  script-driven powers are mediated.

## Shared rules

- **Install:** read-only at `E:\SteamLibrary\steamapps\common\swkotor`; hash sentinels before and
  after each gate run. No original save or install write.
- **No Python in the runtime:** the engine loads NCS, 2DA and GFF directly. Python scripts under
  `tools/` and `engine/scripts/` are verification oracles only.
- **Evidence:** every claim needs a path, test result or log line. Corpus claims cite the engine
  JSONL and the oracle JSON.
- **Stop rule:** three identical failed attempts on one item -> document the exact failure and stop
  that item; do not thrash.

## Running the sprint gates

```powershell
cd engine
cmd /c "scripts\build.bat msvc-release"
$ctest = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
& $ctest --preset msvc-release --output-on-failure
```

Individual proofs (from `engine/`; the checker runs from the repo root):

```powershell
.\build\msvc-release\kotor_engine.exe --module end_m01aa --script-test --null --headless
.\build\msvc-release\kotor_engine.exe --module end_m01aa --dialogue-test --null --headless
.\build\msvc-release\kotor_engine.exe --module end_m01aa --rules-test --null --headless
.\build\msvc-release\kotor_engine.exe --module end_m01aa --systems-test --null --headless
cd ..
python engine\scripts\check_vm_corpus.py --engine engine\build\msvc-release\kotor_engine.exe `
  --corpus artifacts\lanes\04-ncs\extracted\unique `
  --default-oracle artifacts\lanes\s6-01-vm\default_oracle_s6.json `
  --functional-oracle artifacts\lanes\s6-02-actions\functional_oracle_s6.json `
  --out artifacts\lanes\s6-01-vm\parity.json
```
