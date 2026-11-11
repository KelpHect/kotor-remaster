#pragma once

// Stage 1 of the quest tracer (docs/recon/64_QUEST_TRACE.md): a static graph of who
// sets each journal entry and plot global, what gates it and what reads it, built
// from global.jrl, every DLG and every NCS script of the mounted install. Nothing
// here runs a script. Only tags, resrefs and numbers are kept: no game text.

#include "kotor/Formats.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace ke {
class GameSession;
}

namespace ke::quest {

// One precondition: `kind` global|journal|local|item|exists|call|unknown. A `call` or
// `unknown` requirement is opaque: the harness cannot establish it by a state write.
struct Req {
    std::string kind;
    std::string name;
    std::string owner;       // local: self, pc, module, area, tag:<tag>. item: holder.
    std::string op;          // eq ne gt ge lt le true false
    std::int64_t value = 0;
    std::string text;        // A string comparand (global strings).
    bool boolRead = false;   // Read with GetGlobalBoolean: it answers 0 or 1 whatever is stored.
    bool opaque() const { return kind == "call" || kind == "unknown"; }
    std::string key() const;
};
using ReqSet = std::vector<Req>;

// What one action call does. Kinds: journal_set journal_remove global_set local_set
// xp plot_xp item_create item_give item_take create_object start_module end_game
// party_add door_op planet exec_script start_conv.
struct Effect {
    std::string kind;
    std::string name;         // Quest, global, module, resref, door tag, plot label...
    std::string aux;          // Owner or holder, waypoint, door operation.
    std::int64_t value = 0;
    bool valueKnown = false;
    bool nameKnown = false;
    bool flag = false;        // journal_set: bAllowOverrideHigher.
    std::string key() const;
};

struct Site {
    std::uint32_t offset = 0;
    std::string action;
    Effect effect;
    std::vector<ReqSet> paths; // Up to three distinct guard sets that reach this call.
    bool deferred = false;     // Inside a DelayCommand/AssignCommand/ActionDoCommand block.
    bool pathsCapped = false;  // There are more ways to reach it than `paths` holds.
};

struct Read {
    std::string kind; // global journal local item exists
    std::string name;
    std::string owner;
};

struct ScriptFacts {
    std::vector<Site> sites;
    std::vector<ReqSet> truePaths;  // Guards of a path returning non-zero.
    std::vector<ReqSet> falsePaths;
    std::vector<Read> reads;
    bool isConditional = false;
    bool truncated = false; // The path budget ran out; the facts are a lower bound.
    bool decodeFailed = false;
    bool pathsDropped = false;     // The walk stopped following some state after a few visits.
    bool truePathsCapped = false;
    bool tagOfOther = false;       // Some GetTag asks about another object than OBJECT_SELF.
    int cutLine = 0;                  // Where the walk gave up, for KOTOR_QUEST_DIAG.
    std::uint64_t steps = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> bytes; // Kept when it reads GetRunScriptVar, to walk again narrowed.
    std::vector<std::string> names;     // Every constant string but tag lookups, lower case.
    std::vector<std::string> fragments; // Constant strings it builds another string from.
    std::vector<std::string> waypointTags; // Constant tags it looks up with GetWaypointByTag, lower case.
    // Stretches of code entered only by falling through a jump-if-zero on GetIsObjectValid's answer.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> validGuards;
};

// One conversation node, without its text.
struct DlgNode {
    std::string script;
    std::string quest;
    std::int32_t questEntry = 0;
    bool hasQuestEntry = false;
    std::int32_t plotIndex = -1;
    float plotXp = 1.0f;
    std::vector<std::pair<std::uint32_t, std::string>> links; // Target, Active.
};

struct DlgInfo {
    std::string resref;
    std::string module; // Empty: global.
    std::vector<DlgNode> entries;
    std::vector<DlgNode> replies;
    std::vector<std::pair<std::uint32_t, std::string>> starting;
    std::string endConversation;
    std::string endAbort;
};

// A placed instance or a template that can be spawned.
struct ObjectInfo {
    std::string kind;     // creature door placeable trigger encounter store waypoint
    std::string tag;
    std::string resref;
    std::string conversation;
    std::map<std::string, std::string> hooks;
    std::string linkedModule;
    std::string linkedTo;
    kotor::Vec3 position;
    bool placed = false;
};

struct ModuleInfo {
    std::string name;
    std::string planet;
    std::map<std::string, std::string> sceneHooks;
    std::vector<ObjectInfo> objects;
    std::map<std::string, std::size_t> scripts; // lower resref -> index into Graph::facts.
    std::vector<std::string> dialogues;         // lower resrefs in this module's archives.
};

struct JournalEntry {
    std::int32_t id = 0;
    bool end = false;
    float xpPercentage = 0.0f;
};

struct JournalQuest {
    std::string tag;
    std::int32_t plotIndex = -1;
    std::int32_t xp = 0;
    std::vector<JournalEntry> entries;
};

// The object an edge runs on, and where its effect comes from.
struct Host {
    // dlg_entry dlg_reply dlg_end dlg_abort hook scene_hook script.
    std::string kind;
    std::string dlg;
    std::uint32_t node = 0;
    std::string script;       // Script run on the host; empty for a DLG node's own fields.
    std::string hook;
    std::string ownerTag;     // A hook's object, or a placed conversation owner.
    std::string ownerResref;  // Template that can spawn the owner.
    std::string ownerKind;
    std::string linkActive;   // Immediate condition of the node (first incoming link).
    std::string id() const;
};

struct Edge {
    std::string id;
    std::string module;
    std::string planet;
    Host host;
    std::string siteScript;   // The script holding the call (may be an ExecuteScript target).
    std::uint32_t siteOffset = 0;
    Effect effect;
    std::vector<ReqSet> gates;        // Alternatives; site guards + chain guards.
    std::vector<std::string> pathActive; // Active scripts on a shortest route to the node.
    bool deferred = false;
    bool dlgLinked = true;    // False: no route from a starting link to the node.
    bool orphan = false;      // A script with no hook, DLG or ExecuteScript caller found.
    bool debug = false;       // A developer test script (qa_*, k_qa*, test*): never a plot route.
    std::string dead;         // Why no run can reach the site: every path contradicts a fact.
};

struct Unresolved {
    std::string module;
    std::string script;
    std::string action;
    std::uint32_t offset = 0;
    std::string detail;
};

struct ModuleEdge {
    std::string from;
    std::string to;
    std::string via;   // edge id, or door:<tag> / trigger:<tag>.
    std::string kind;  // script door trigger
    std::string aux;   // Waypoint or LinkedTo.
};

struct ChainStep {
    std::string from;
    std::string to;
    std::string via;
    std::string kind;
    std::vector<std::string> gates; // Gate keys of the chosen route.
};

struct DoorOpener {
    std::string module;
    std::string script;
    std::string operation; // SetLocked:0, ActionOpenDoor, ActionUnlockObject.
};

struct Graph {
    std::vector<JournalQuest> quests;
    std::vector<ModuleInfo> modules;
    std::vector<ScriptFacts> facts;
    std::vector<ObjectInfo> globalObjects; // Templates outside the modules.
    std::map<std::string, std::vector<std::string>> itemTemplateHomes; // Item template to the modules holding it.
    std::map<std::string, std::size_t> globalScripts;    // lower resref -> facts index.
    std::map<std::string, DlgInfo> dialogues;            // "module|resref", or "|resref".
    std::vector<Edge> edges;
    std::vector<Unresolved> unresolved;
    std::vector<ModuleEdge> moduleEdges;
    std::map<std::string, std::vector<DoorOpener>> doorOpeners; // lower door tag.
    std::set<std::string> declaredGlobals;                       // globalcat.2da
    std::set<std::string> mainPath;                              // Modules on the main plot.
    std::vector<std::string> checkpoints; // The plot's ordered stops, before the endings fork.
    std::size_t unreachableNames = 0;     // Edges whose owner, conversation or script nothing can start.
    std::size_t runVarWalks = 0;          // Walks narrowed by ExecuteScript's constant, and how many
    std::size_t runVarWalksCut = 0;       // of them ran out of budget and so ruled nothing out.
    std::vector<ChainStep> chainLight;
    std::vector<ChainStep> chainDark;
    std::vector<std::string> endingModules;
    std::string chainResult;
    std::vector<std::string> chainMissingLegs;
    std::vector<std::string> chainGatesNoSetter;
    // Reporting flags.
    std::vector<std::string> entriesNoSetter;         // quest=entry
    std::vector<std::string> entriesNotInJournal;     // quest=entry set by code, not in global.jrl
    std::vector<std::string> questsUnknown;           // quest tags set by code, not in global.jrl
    std::vector<std::string> gatesNeverSet;           // global names read and never set
    std::vector<std::string> caseMismatchedGlobals;   // Same name, different case
    std::vector<std::string> undeclaredGlobals;       // Used by code, not in globalcat.2da
    std::map<std::string, std::vector<std::string>> journalReaders; // "quest=entry" -> scripts
    std::size_t scriptCount = 0;
    std::size_t scriptsTruncated = 0;
    std::size_t scriptsFailed = 0;
    std::size_t dlgCount = 0;
    std::size_t nodeCount = 0;
};

std::string planetOf(const std::string& module);

// `install` must have no module bound: the walk opens each module's archives itself.
Graph buildGraph(kotor::Install& install);
void writeGraphJson(const Graph& graph, const std::filesystem::path& path);

// Analyses one script without running it.
ScriptFacts analyzeScript(const std::vector<std::uint8_t>& bytes);
// The instructions a run can reach with GetRunScriptVar pinned to what ExecuteScript passed.
// Not complete: says nothing.
struct RunVarReach {
    bool complete = false;
    std::string why; // What stopped it, for KOTOR_QUEST_DIAG.
    std::unordered_set<std::uint32_t> offsets;
};
RunVarReach reachForRunVar(const std::vector<std::uint8_t>& bytes, std::int64_t runVar);

// --quest-trace: stages 1 to 3 and the gate (QuestTrace.cpp).
struct QuestTraceOptions {
    std::string outDir;
    std::string baseline;
    std::string modules;          // Comma list; empty traces every module.
    std::string breakName;        // Test-only fault for the negative check.
    bool graphOnly = false;
    double deadlineSeconds = 1100.0; // A safety stop, not a budget: reaching it fails the gate.
};
int runQuestTrace(GameSession& game, const QuestTraceOptions& options);

// --conversation-flow and --plot-replay (QuestFlow.cpp).
struct FlowOptions {
    std::string outDir;
    std::string baseline;
    std::string modules;   // Conversation flow only: a comma list; empty means the main path.
    std::string breakName; // The negative check.
    double deadlineSeconds = 780.0;
    std::function<void()> onPlotState; // Plot replay: called at each module reached.
};
int runConversationFlow(GameSession& game, const FlowOptions& options);
int runPlotReplay(GameSession& game, const FlowOptions& options);

} // namespace ke::quest
