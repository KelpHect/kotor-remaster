# Walk steps (`--input-script`)

A walk script plays the engine through the player's own paths: the real pathfinder, the default action on an
object, the conversation panel's choices, the attack command. Steps never teleport and never write quest state.
Steps run in order; each holds the script until it reports done. A failed step ends the run (unless
`--walk-continue`). Timeouts are in game (simulated) seconds. `#` starts a comment. The older timed steps
(`wait`, `key`, `hold`, `shotas`, `waitfor ...`, `script`) still work in the same file.

    ctest: kotor_engine --new-game --input-script engine/tests/walk/opening.walk --null --headless --fixed-dt 0.0166667 \
                        --no-audio --no-movies --saves-dir <d> --settings-file <f>

## Steps

| step | meaning |
|---|---|
| `goto <waypoint\|tag\|x,y[,z]> [stop=<m>] [walk] [through] [timeout=<s>]` | Walk the leader there with the real pathfinder (`moveTo`). `through`: the walk goes through a linked door that stands open, and the step is OK when the module changes under it. A tag prefers a waypoint, else the nearest object of that tag. Waits for control first (no conversation, cutscene, movie, minigame, party screen). Retries up to 6 times when a scene takes the control mid-walk. FAIL reasons: `unreachable`, `blocked` (names the door, open/locked/key), `stuck` (no 0.3 m progress in 6 s), `ended-short`, `timeout`, `unknown-target`. |
| `use <tag> [timeout=<s>]` | The player's default action on a door or placeable: walk into reach, then open / use / transition (`performDefaultAction`). OK outcomes: opened, closed, transition, looted, used. FAIL: `locked` (with key name), `refused`, `walk-ended-short`, `not-usable`, `stuck`, `timeout`. |
| `talk <tag> [timeout=<s>]` | The same for a creature: walk to it and start its conversation (through its OnDialogue hook when it has one). OK once a conversation is up. FAIL: `no-conversation`, `hostile`. |
| `opendoor <tag> [must]` | ActionOpenDoor as the controlled creature, in reach (for doors the mouse cannot target; a locked door runs its OnFailToOpen hook). `must`: only an opened door passes. |
| `leader <player\|n>`, `unlock <tag>` (Security, up to 25 tries), `loot`, `equip all` | Party and inventory helpers. |
| `dialogue <policy> [timeout=<s>]` | Run the conversation that is up, or starts next, to its end with the policy; OK when it has ended. FAIL: `stalled` (30 s without progress), `timeout`. |
| `autodialogue <policy>` / `autodialogue off` | Answer every conversation that starts while any later step runs (scripted cutscenes, arrival conversations). |
| `fight [range=<m>] [timeout=<s>]` | Combat with the real rules: while an enemy is within range (default 20 m) the controlled creature attacks the nearest (`attackNearestHostile`, the attack command the HUD sends) and the party fights on its own AI. OK after 1.5 s with no enemy in range. FAIL: `party-wiped`, `timeout` (lists remaining enemies and distance). |
| `module <name> [timeout=<s>]` | Wait until the game is in this module. |
| `expect module <name>` | The state assertions: `expect global <NAME> <op> <n>`, `expect journal <TAG> <op> <n>`, `expect alive\|dead <tag>`, `expect near <tag> <m>`, `expect party <NPC tag>`, `expect credits`. Ops: `= != < > <= >=`. |
| `idle <seconds>` | Let the game run (services conversations from `autodialogue`). |
| `partyselect` | Wait for the party-selection screen a script opened and press Accept (the exit script runs). |
| `list [kinds=door,creature,...] [range=<m>]` | Print the objects around the leader (route authoring; changes nothing). |
| `note <text>` | A line in the report. |

### Dialogue policies

| policy | meaning |
|---|---|
| `first` (default) | The first reply not yet said in this conversation, else the last one (a hub's way out is its last reply); "continue" and "end" entries take their only option. A conversation past 300 nodes is reported as `stalled`. |
| `index <i> <i> ...` | One 0-based position in the shown list per decision that has a reply to say; falls back to first when used up or out of range. |
| `hint <dlg>:<entry\|*>=<i>,...` | Position by conversation resref and entry number; `*` matches any entry; falls back to first. |
| `hintfile=<path>` | Hints one per line: `<dlg> <entry\|*> <i>`. |

`dialogue ... optional` passes when no conversation starts within its timeout.

Add `noskip` to let spoken lines finish instead of clicking through them.

## Output (stable, one line each)

    WALK step <n> OK   <line> | t=<s>s module=<m> pos=<x>,<y>,<z> [detail]
    WALK step <n> FAIL <line> | t=<s>s module=<m> pos=... reason=<why> ...
    WALK dlg step=<n> resref=<dlg> entry=<node> options=<k> sayable=<k> pick=<i> reply=<node|end> via=<first|index|hint|continue>
    WALK load step=<n> from=<m> module=<m> ms=<wall ms of the frame that loaded it>
    WALK script-fail step=<n> module=<m> kind=<status|missing|decode|refused> script=<s> line=<log line>
    WALK frames count=.. avg_ms=.. p50_ms=.. p99_ms=.. max_ms=.. stalls_over_250ms=.. stall_total_ms=..
    WALK resource-misses distinct=.. total=..   then   WALK miss kind=<ext|tex> resref=<r> count=<n>   (top 80)
    WALK summary steps=.. ok=.. fail=.. finished=<0|1> exit="..." sim_s=.. last_module=..
    WALK RESULT PASS | WALK RESULT FAIL first_failed="<line>"

Exit code 40 on any FAIL or when the run ended (time limit, window closed) before the script did. Dialogue lines
carry resrefs and node numbers only, never the text.

## Flags

| flag | meaning |
|---|---|
| `--new-game` | Start a new game: default quick-built character (male Soldier), `end_m01aa` through the same `startNewGame` the front end calls. The continuous run starts here. |
| `--walk-godmode` | Route discovery only: a party member at 60% of its hit points or less is healed to full (it only prevents party death; damage, kills and fights are otherwise the game's). Never use it in a test; damage, kills and fights are otherwise the game's. |
| `--walk-continue` | Keep going after a failed step. |
| `--start-wp <tag>`, `--seed-file <f>` | Per-module probing only (never in the continuous run): stand the leader on a waypoint after the load; `--seed-file` lines `global NAME = N`, `journal TAG = N`, `party A,B`, `gold N`, `xp N`. |
