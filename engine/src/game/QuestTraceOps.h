#pragma once

// The session harness shared by the quest trace and the sequence modes (QuestTrace.cpp,
// QuestFlow.cpp). It is a friend of GameSession.
#include "game/GameSession.h"

#include "game/Pathfinding.h"
#include "game/QuestGraph.h"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <queue>
#include <limits>
#include <deque>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace ke {

using quest::Edge;
using quest::Effect;
using quest::Graph;
using quest::Req;
using quest::ReqSet;

inline std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

// Dead: shipped code or data shows no run reaches the effect. Never counted as a pass.
enum class Status { Pass, Fail, Cant, Dead };

inline const char* statusText(Status status)
{
    switch (status) {
    case Status::Pass: return "pass";
    case Status::Fail: return "fail";
    case Status::Cant: return "cant";
    case Status::Dead: return "dead";
    }
    return "cant";
}

struct EdgeResult {
    Status status = Status::Cant;
    std::string reason;
};

struct NavRow {
    std::string module;
    std::string subject;
    std::string kind;
    std::string status; // reachable reachable_via_doors door_no_opener unreachable no_mesh not_placed
    std::string from;
    std::string doors;
    bool mainPath = false;
};

inline std::size_t peakMemoryMb()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return counters.PeakWorkingSetSize / (1024 * 1024);
    }
#endif
    return 0;
}

inline bool satisfies(std::int64_t value, const std::string& op, std::int64_t target)
{
    if (op == "true") return value != 0;
    if (op == "false") return value == 0;
    if (op == "eq") return value == target;
    if (op == "ne") return value != target;
    if (op == "gt") return value > target;
    if (op == "ge") return value >= target;
    if (op == "lt") return value < target;
    if (op == "le") return value <= target;
    return false;
}

// The harness. A friend of GameSession: it drives the real conversation, hook, script
// and action paths. It writes state only to establish a recorded precondition.
struct QuestTraceOps {
    GameSession& g;
    const Graph& graph;
    const quest::ModuleInfo* module = nullptr;

    // ---- the pristine module ------------------------------------------------------------

    struct Pristine {
        script::World world{0x4B4F544F52ULL};
        std::vector<kotor::Entity> entities;
        std::size_t meshes = 0;
        std::vector<std::uint32_t> entityObjects;
        std::optional<WorldSim> sim;
        CharacterSheet character;
        std::int32_t gold = 0;
        std::int32_t hp = 0;
        std::int32_t maxHp = 0;
        kotor::Vec3 position;
        std::vector<CompanionState> companions;
        std::vector<std::string> partyTags;
        std::string leaderTag;
        PartyTable party;
        std::vector<MerchantState> merchants;
        std::vector<EncounterState> encounters;
        std::vector<bool> triggerInside;
    };
    Pristine pristine;

    // Every module starts from a blank plot state: a harness choice, so one edge's
    // preconditions never leak into the next module.
    bool load(const std::string& name, const std::vector<const Edge*>& edges, std::string& error)
    {
        g.journal_.clear();
        g.scriptWorld_.globals.clear();
        g.sim_->snapshots.clear();
        g.sim_->carried.clear();
        g.pendingModule_.clear();
        g.sim_->pendingLinkedTo.clear();
        g.sim_->pendingMovies.clear();
        g.activeDialogue_.reset();
        try {
            g.loadModule(name, nullptr, {});
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
        g.sim_->snapshots.clear();
        g.loadJournalData();
        g.loadPlotTable();
        settle();
        // What OnEnter deferred happens before anyone can talk: k_pebn_pophawk destroys the
        // placed Carth at 0.5 s and respawns him at 2 s, and a closure on the old object would
        // be dropped with it. Left pending, it fired during the first pump of every run.
        pump([]() { return false; });
        settle();
        // A conversation or hook owner the module only knows as a template, once per
        // template: spawning one per run cost gigabytes of model data on large modules.
        std::set<std::string> spawned;
        for (const Edge* edge : edges) {
            const quest::Host& host = edge->host;
            if (host.ownerKind == "creature" && !host.ownerResref.empty() && spawned.insert(host.ownerResref).second
                && findEntity(host) == static_cast<std::size_t>(-1)) {
                g.spawnScriptCreature(host.ownerResref, g.position_, 0.0f);
            }
        }
        settle();
        capture();
        dialogues.clear();
        return true;
    }

    // Lets the loading scripts' own deferred work finish, then drops any cutscene or
    // transition they started so each edge begins from the same place.
    void settle()
    {
        g.clearDialogueState();
        g.pendingModule_.clear();
        g.sim_->pendingLinkedTo.clear();
        g.sim_->pendingMovies.clear();
        g.moviePlaying_ = false;
        g.movieQueue_.clear();
    }

    void capture()
    {
        pristine.world = g.scriptWorld_;
        pristine.entities = g.scene_.entities;
        pristine.meshes = g.scene_.entityMeshes.size();
        pristine.entityObjects = g.entityObjects_;
        // The journal table, plot table and decoded scripts never change in a module.
        auto journal = std::move(g.sim_->journal);
        auto plot = std::move(g.sim_->plot);
        auto decoded = std::move(g.sim_->decoded);
        pristine.sim = *g.sim_;
        g.sim_->journal = std::move(journal);
        g.sim_->plot = std::move(plot);
        g.sim_->decoded = std::move(decoded);
        pristine.character = g.character_;
        pristine.gold = g.gold_;
        pristine.hp = g.playerHp_;
        pristine.maxHp = g.playerMaxHp_;
        pristine.position = g.position_;
        pristine.companions = g.companions_;
        pristine.partyTags = g.partyTags_;
        pristine.leaderTag = g.leaderTag_;
        pristine.party = g.party_;
        pristine.merchants = g.merchants_;
        pristine.encounters = g.encounterStates_;
        pristine.triggerInside = g.triggerInside_;
    }

    void restore()
    {
        answers.clear();
        globalWrites.clear();
        droppedGlobals.clear();
        g.activeDialogue_.reset();
        g.dialogueView_ = {};
        g.scriptWorld_ = pristine.world;
        g.scene_.entities = pristine.entities;
        if (g.scene_.entityMeshes.size() > pristine.meshes) {
            g.scene_.entityMeshes.erase(g.scene_.entityMeshes.begin() + static_cast<std::ptrdiff_t>(pristine.meshes),
                                        g.scene_.entityMeshes.end());
        }
        g.entityObjects_ = pristine.entityObjects;
        auto journal = std::move(g.sim_->journal);
        auto plot = std::move(g.sim_->plot);
        auto decoded = std::move(g.sim_->decoded);
        *g.sim_ = *pristine.sim;
        g.sim_->journal = std::move(journal);
        g.sim_->plot = std::move(plot);
        g.sim_->decoded = std::move(decoded);
        g.journal_.clear();
        g.character_ = pristine.character;
        g.gold_ = pristine.gold;
        g.playerHp_ = pristine.hp;
        g.playerMaxHp_ = pristine.maxHp;
        g.playerDead_ = false;
        g.position_ = pristine.position;
        g.companions_ = pristine.companions;
        g.partyTags_ = pristine.partyTags;
        g.leaderTag_ = pristine.leaderTag;
        g.party_ = pristine.party;
        g.merchants_ = pristine.merchants;
        g.encounterStates_ = pristine.encounters;
        g.triggerInside_ = pristine.triggerInside;
        g.activeMerchant_ = -1;
        g.combatTarget_ = UINT32_MAX;
        g.pendingModule_.clear();
        g.moviePlaying_ = false;
        g.movieQueue_.clear();
        g.pazaak_.reset();
        g.pazaakStage_ = GameSession::PazaakStage::None;
        g.scriptLog_.clear();
        if (g.nav_) {
            g.nav_->actors.clear();
        }
    }

    // ---- preconditions ------------------------------------------------------------------

    struct Established {
        std::vector<std::string> opaque;      // Conditions no state write can establish.
        std::vector<std::string> unsatisfied; // Conditions this harness could not meet.
        std::vector<std::string> mocked;      // Engine queries answered for this run.
        ScriptContext context;
        std::set<std::string> touched;        // "global|name", "journal|name".
    };

    // One concrete try at an edge the first run could not settle.
    struct Variant {
        std::map<std::string, std::int64_t> forced; // Variable -> value, in place of solving.
        std::set<std::string> realCalls;            // Queries left to their real handlers.
        bool allReal = false;                       // Every query is: the directed search.
        std::string sequenceCall;                   // This query answers "no" until call sequenceHit.
        int sequenceHit = -1;
    };
    const Variant* variant = nullptr;

    std::uint32_t resolveOwner(const std::string& owner, std::uint32_t self) const
    {
        if (owner == "self" || owner.empty()) return self;
        if (owner == "pc") return g.scriptWorld_.pc;
        if (owner == "module") return g.sim_->moduleObject;
        if (owner == "area") return g.sim_->areaObject;
        if (owner.rfind("tag:", 0) == 0) return g.scriptWorld_.byTag(owner.substr(4), 0);
        // The party leader unless a script reorders the party, which no run here does.
        if (owner == "call:GetPartyMemberByIndex") return g.scriptWorld_.pc;
        return script::kObjectInvalid;
    }

    // Finds a value meeting every constraint on one variable.
    static std::optional<std::int64_t> solve(const std::vector<const Req*>& reqs)
    {
        std::vector<std::int64_t> candidates{1, 0};
        for (const Req* req : reqs) {
            candidates.push_back(req->value);
            candidates.push_back(req->value + 1);
            candidates.push_back(req->value - 1);
        }
        // The value named by an equality comes first, then the rest.
        for (const Req* req : reqs) {
            if (req->op == "eq") {
                candidates.insert(candidates.begin(), req->value);
            }
        }
        for (const std::int64_t candidate : candidates) {
            if (std::all_of(reqs.begin(), reqs.end(), [&](const Req* req) {
                    return satisfies(candidate, req->op, req->value);
                })) {
                return candidate;
            }
        }
        return std::nullopt;
    }

    std::uint32_t spawnForTag(const std::string& tag)
    {
        if (module == nullptr) {
            return script::kObjectInvalid;
        }
        const std::string wanted = lowered(tag);
        for (const quest::ObjectInfo& object : module->objects) {
            if (object.kind == "creature" && lowered(object.tag) == wanted) {
                return g.spawnScriptCreature(object.resref, g.position_, 0.0f);
            }
        }
        return script::kObjectInvalid;
    }

    // ---- answers to engine queries --------------------------------------------------------

    // A script looping until a query changes must still end: past this many calls in a
    // run, the query goes to its real handler.
    static constexpr std::size_t kMaxAnswers = 64;
    struct Answer {
        std::vector<script::Value> sequence;
        std::size_t next = 0;
    };
    std::map<std::uint16_t, Answer> answers;
    std::vector<std::pair<std::string, std::int64_t>> globalWrites;
    // Names a script used with no matching catalog row: retail drops the write and reads 0.
    std::set<std::string> droppedGlobals;
    std::map<std::string, std::pair<bool, kotor::Dlg>> dialogues;
    double timeRestore = 0.0;
    double timeRun = 0.0;
    double timePump = 0.0;
    std::ofstream* failLog = nullptr;
    std::array<ActionHandler, kScriptActionCount> originals{};
    std::vector<std::uint16_t> installed;
    static inline QuestTraceOps* current = nullptr;

    static std::optional<std::vector<script::Value>> answeringAction(ActionCall& call)
    {
        QuestTraceOps& ops = *current;
        if (call.id >= kGetGlobalBoolean && call.id <= kSetGlobalNumber) {
            const script::GlobalKind kind = call.id <= kSetGlobalBoolean ? script::GlobalKind::Boolean : script::GlobalKind::Number;
            if (!ops.g.runtime_->globalDeclared(call.stringArg(0), kind)) {
                ops.droppedGlobals.insert(lowered(call.stringArg(0)));
            }
        }
        if (call.id == kSetGlobalBoolean || call.id == kSetGlobalNumber) {
            const std::int64_t value = call.intArg(1);
            ops.globalWrites.emplace_back(lowered(call.stringArg(0)), call.id == kSetGlobalBoolean && value != 0 ? 1 : value);
        }
        if (ops.search != nullptr) {
            return ops.searchAction(call);
        }
        if (const auto found = ops.answers.find(call.id);
            found != ops.answers.end() && found->second.next < kMaxAnswers) {
            Answer& answer = found->second;
            const std::size_t at = std::min(answer.next, answer.sequence.size() - 1);
            ++answer.next;
            return std::vector<script::Value>{answer.sequence[at]};
        }
        if (const ActionHandler original = ops.originals[call.id]; original != nullptr) {
            return original(call);
        }
        return std::nullopt;
    }

    static std::optional<std::uint16_t> actionNamed(const std::string& name)
    {
        for (const script::ActionSpec& spec : script::actionTable()) {
            if (name == spec.name) {
                return spec.id;
            }
        }
        return std::nullopt;
    }

    // Routes every query some gate names through answeringAction. With no answer set for
    // the run, the query goes to its real handler.
    void installAnswers()
    {
        current = this;
        std::set<std::uint16_t> ids{kGetIsObjectValid, kGetGlobalBoolean, kSetGlobalBoolean, kGetGlobalNumber, kSetGlobalNumber};
        for (const Edge& edge : graph.edges) {
            for (const ReqSet& reqs : edge.gates) {
                for (const Req& req : reqs) {
                    if (req.kind == "call") {
                        if (const auto id = actionNamed(req.name.substr(0, req.name.find('~'))); id.has_value()) {
                            ids.insert(*id);
                        }
                    }
                }
            }
        }
        for (const script::ActionSpec& spec : script::actionTable()) {
            if (searchable(spec)) {
                ids.insert(spec.id);
            }
        }
        for (const std::uint16_t id : ids) {
            originals[id] = g.sim_->handlers.get(id);
            g.sim_->handlers.add(id, &QuestTraceOps::answeringAction);
            installed.push_back(id);
        }
    }

    // Hands the actions back, so another harness can be installed in the same session.
    void uninstallAnswers()
    {
        // An action with no handler of its own goes back to having none.
        for (const std::uint16_t id : installed) {
            g.sim_->handlers.add(id, originals[id]);
        }
        installed.clear();
        originals = {};
        current = nullptr;
    }

    // ShipBuild picks debug code, which is not a query to answer: the build decides it.
    bool answerQuery(const std::string& name, const std::vector<const Req*>& group, std::uint32_t self)
    {
        (void)self;
        const std::size_t tilde = name.find('~');
        const std::string left = name.substr(0, tilde);
        const std::string right = tilde == std::string::npos ? std::string{} : name.substr(tilde + 1);
        const auto id = actionNamed(left);
        if (!id.has_value() || left == "ShipBuild") {
            return false;
        }
        const script::ReturnKind returns = script::actionSpec(*id).returnKind;
        Answer answer;
        if (returns == script::ReturnKind::Object) {
            std::uint32_t target = script::kObjectInvalid;
            if (right == "pc") {
                target = g.scriptWorld_.pc;
            } else if (right.rfind("tag:", 0) == 0) {
                target = g.scriptWorld_.byTag(right.substr(4), 0);
            }
            if (target == script::kObjectInvalid) {
                return false;
            }
            for (const Req* req : group) {
                answer.sequence.push_back(script::Value::makeObject(req->op == "eq" ? target : script::kObjectInvalid));
            }
        } else if (returns == script::ReturnKind::String) {
            for (const Req* req : group) {
                if (req->op != "eq" && req->op != "ne") {
                    return false;
                }
                answer.sequence.push_back(script::Value::makeString(req->op == "eq" ? req->text : std::string{}));
            }
        } else if (returns == script::ReturnKind::Int && right.empty()) {
            if (const auto value = solve(group); value.has_value()) {
                answer.sequence.push_back(script::Value::makeInt(*value));
            } else {
                // One query asked twice wanting different answers: answer in the order asked.
                for (const Req* req : group) {
                    const auto single = solve({req});
                    if (!single.has_value()) {
                        return false;
                    }
                    answer.sequence.push_back(script::Value::makeInt(*single));
                }
            }
        } else {
            return false;
        }
        answers[*id] = std::move(answer);
        return true;
    }

    // One query asked several times, wanting a yes once: an else-if chain over a value. Calls
    // before `hit` and after it get a value that fails the wanted test.
    bool answerSequence(const std::string& name, const std::vector<const Req*>& group, int hit)
    {
        const std::size_t tilde = name.find('~');
        const auto id = actionNamed(name.substr(0, tilde));
        if (!id.has_value() || hit < 0 || tilde != std::string::npos) {
            return false;
        }
        const script::ReturnKind returns = script::actionSpec(*id).returnKind;
        script::Value yes;
        script::Value no;
        if (returns == script::ReturnKind::Int) {
            const auto value = solve(group);
            if (!value.has_value()) {
                return false;
            }
            std::optional<std::int64_t> other;
            for (const std::int64_t candidate : {std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}, *value + 1}) {
                if (!std::all_of(group.begin(), group.end(),
                                 [&](const Req* req) { return satisfies(candidate, req->op, req->value); })) {
                    other = candidate;
                    break;
                }
            }
            if (!other.has_value()) {
                return false;
            }
            yes = script::Value::makeInt(*value);
            no = script::Value::makeInt(*other);
        } else if (returns == script::ReturnKind::String && group.front()->op == "eq") {
            yes = script::Value::makeString(group.front()->text);
            no = script::Value::makeString(std::string{});
        } else {
            return false;
        }
        Answer answer;
        answer.sequence.assign(static_cast<std::size_t>(hit) + 2, no);
        answer.sequence[static_cast<std::size_t>(hit)] = yes;
        answers[*id] = std::move(answer);
        return true;
    }

    void establish(const ReqSet& reqs, std::uint32_t self, Established& out)
    {
        std::map<std::string, std::vector<const Req*>> byVariable;
        std::map<std::string, std::vector<const Req*>> byCall;
        for (const Req& req : reqs) {
            if (req.opaque()) {
                if (req.name == "GetUserDefinedEventNumber" && (req.op == "eq")) {
                    out.context["userDefinedNumber"] = static_cast<std::uint32_t>(req.value);
                } else if (req.kind == "call" && !req.name.empty()) {
                    byCall[req.name].push_back(&req);
                } else {
                    out.opaque.push_back(req.name.empty() ? std::string("unknown") : req.name);
                }
                continue;
            }
            byVariable[req.kind + "|" + req.owner + "|" + req.name].push_back(&req);
        }
        for (const auto& [name, group] : byCall) {
            // The value of a pure function follows from its arguments: there is nothing to answer.
            if (pureQuery(name.substr(0, name.find('~')))) {
                continue;
            }
            if (variant != nullptr && variant->allReal) {
                continue;
            }
            if (name == "pc~self") {
                // Not answered: the run happens on the PC (runGroup), or the owner is not the PC.
                out.mocked.push_back(name);
                continue;
            }
            if (variant != nullptr && variant->realCalls.contains(name)) {
                continue;
            }
            if (variant != nullptr && variant->sequenceCall == name && answerSequence(name, group, variant->sequenceHit)) {
                out.mocked.push_back(name);
                continue;
            }
            if (answerQuery(name, group, self)) {
                out.mocked.push_back(name);
            } else {
                out.opaque.push_back(name);
            }
        }
        for (const auto& [variable, group] : byVariable) {
            const Req& first = *group.front();
            if (first.kind == "global" && !first.text.empty()) {
                const bool wantEqual = first.op == "eq";
                g.scriptWorld_.setGlobal(first.name, script::Value::makeString(wantEqual ? first.text : std::string{}));
                out.touched.insert("global|" + lowered(first.name));
                continue;
            }
            std::optional<std::int64_t> value = solve(group);
            if (variant != nullptr) {
                if (const auto forced = variant->forced.find(variable); forced != variant->forced.end()) {
                    value = forced->second;
                }
            }
            if (!value.has_value() && first.kind == "exists") {
                // Valid, then gone: an earlier test's short circuit reads the same object, and the walk
                // keeps both reads. A killed creature is destroyed (004e0ac0 posts event 11; 004fece0).
                value = 0;
            }
            if (!value.has_value()) {
                // One path wanting two values: a condition and a later branch that can
                // never hold together, so no run ever takes this path.
                out.unsatisfied.push_back("infeasible|" + variable);
                continue;
            }
            if (first.kind == "global") {
                g.scriptWorld_.setGlobal(first.name, script::Value::makeInt(*value));
                out.touched.insert("global|" + lowered(first.name));
            } else if (first.kind == "journal") {
                if (*value == 0) {
                    g.removeJournalEntry(first.name);
                } else {
                    g.addJournalEntry(first.name, static_cast<std::int32_t>(*value));
                }
                out.touched.insert("journal|" + lowered(first.name));
            } else if (first.kind == "local") {
                const std::uint32_t object = resolveOwner(first.owner, self);
                if (object == script::kObjectInvalid) {
                    out.unsatisfied.push_back(variable);
                    continue;
                }
                std::int64_t index = 0;
                try {
                    index = std::stoll(first.name);
                } catch (const std::exception&) {
                    out.unsatisfied.push_back(variable);
                    continue;
                }
                // SetLocalBoolean and SetLocalNumber use separate slots; a read of
                // either kind is satisfied by writing both.
                std::vector<std::vector<script::Value>> args{{script::Value::makeObject(object)},
                                                             {script::Value::makeInt(index)},
                                                             {script::Value::makeInt(*value)}};
                if (index >= 0 && index < 96) {
                    args[2] = {script::Value::makeInt(*value != 0 ? 1 : 0)};
                    g.actionAsForTest(self, kSetLocalBoolean, args);
                    args[2] = {script::Value::makeInt(*value)};
                }
                g.actionAsForTest(self, kSetLocalNumber, args);
                // Scripts find such an owner by nearest, so every copy of the tag gets it.
                if (first.owner.rfind("tag:", 0) == 0) {
                    const std::string tag = first.owner.substr(4);
                    for (std::int64_t nth = 1;; ++nth) {
                        const std::uint32_t copy = g.scriptWorld_.byTag(tag, nth);
                        if (copy == script::kObjectInvalid) {
                            break;
                        }
                        args[0] = {script::Value::makeObject(copy)};
                        if (index >= 0 && index < 96) {
                            args[2] = {script::Value::makeInt(*value != 0 ? 1 : 0)};
                            g.actionAsForTest(self, kSetLocalBoolean, args);
                            args[2] = {script::Value::makeInt(*value)};
                        }
                        g.actionAsForTest(self, kSetLocalNumber, args);
                    }
                }
            } else if (first.kind == "item") {
                if (*value != 0) {
                    const std::uint32_t holder = resolveOwner(first.owner, self);
                    const std::uint32_t item = holder == script::kObjectInvalid
                        ? script::kObjectInvalid
                        : g.giveItem(lowered(first.name), holder, 1);
                    if (item == script::kObjectInvalid) {
                        out.unsatisfied.push_back(variable);
                    }
                }
            } else if (first.kind == "exists") {
                const std::uint32_t object = g.scriptWorld_.byTag(first.name, 0);
                const bool present = object != script::kObjectInvalid && g.scriptWorld_.valid(object);
                if (*value != 0 && !present) {
                    if (looksUpAsWaypoint(first.name)) {
                        out.unsatisfied.push_back(variable); // Not placed here: only the real lookup speaks.
                    } else if (spawnForTag(first.name) == script::kObjectInvalid) {
                        Answer yes;
                        yes.sequence.push_back(script::Value::makeInt(1));
                        answers[kGetIsObjectValid] = std::move(yes);
                        out.mocked.push_back("exists:" + first.name);
                    }
                } else if (*value == 0 && present) {
                    if (script::GameObject* held = g.scriptWorld_.get(object); held != nullptr) {
                        held->valid = false;
                    }
                    const std::size_t entity = ScriptApi{g}.entityFor(object);
                    if (entity != static_cast<std::size_t>(-1)) {
                        g.scene_.entities[entity].dead = true;
                    }
                }
            }
        }
    }

    // ---- running a host -----------------------------------------------------------------

    bool looksUpAsWaypoint(const std::string& tag)
    {
        if (waypointLookups.empty()) {
            for (const quest::ScriptFacts& facts : graph.facts) {
                waypointLookups.insert(facts.waypointTags.begin(), facts.waypointTags.end());
            }
            waypointLookups.insert(std::string{});
        }
        return waypointLookups.contains(lowered(tag));
    }
    std::set<std::string> waypointLookups;

    static constexpr std::uint16_t kSetLocalBoolean = 680;
    static constexpr std::uint16_t kSetLocalNumber = 682;
    static constexpr std::uint16_t kGetIsObjectValid = 42;
    static constexpr std::uint16_t kGetGlobalBoolean = 578;
    static constexpr std::uint16_t kSetGlobalBoolean = 579;
    static constexpr std::uint16_t kGetGlobalNumber = 580;
    static constexpr std::uint16_t kSetGlobalNumber = 581;

    std::size_t findEntity(const quest::Host& host)
    {
        if (!host.dlg.empty()) {
            for (std::size_t i = 0; i < g.scene_.entities.size(); ++i) {
                const kotor::Entity& entity = g.scene_.entities[i];
                if (!entity.dead && lowered(entity.conversation) == host.dlg) {
                    return i;
                }
            }
        }
        if (!host.ownerTag.empty()) {
            if (const std::size_t index = g.findEntity(host.ownerTag); index != static_cast<std::size_t>(-1)) {
                return index;
            }
        }
        // A placed instance keeps the template's hook under its own tag.
        if (!host.hook.empty() && !host.script.empty()) {
            for (std::size_t i = 0; i < g.scene_.entities.size(); ++i) {
                const kotor::Entity& entity = g.scene_.entities[i];
                if (const auto hook = entity.scripts.find(host.hook);
                    !entity.dead && hook != entity.scripts.end() && lowered(hook->second) == lowered(host.script)) {
                    return i;
                }
            }
        }
        return static_cast<std::size_t>(-1);
    }

    // The object an edge runs on: a placed one, else a spawn of its template, else the PC.
    std::uint32_t ownerFor(const quest::Host& host, bool& fallback)
    {
        fallback = false;
        if (host.kind == "scene_hook") {
            return host.hook.rfind("Mod_", 0) == 0 ? g.sim_->moduleObject : g.sim_->areaObject;
        }
        const std::size_t index = findEntity(host);
        if (index != static_cast<std::size_t>(-1)) {
            return g.objectForEntity(index);
        }
        fallback = host.kind != "script";
        return g.scriptWorld_.pc;
    }

    struct RunOutcome {
        std::optional<bool> condition;
        bool hostMissing = false;
        std::string missing;
        bool threw = false;
        std::string error;
    };

    RunOutcome runHost(const quest::Host& host, std::uint32_t owner, const ScriptContext& extra)
    {
        RunOutcome out;
        const std::uint32_t pc = g.scriptWorld_.pc;
        try {
            if (!host.dlg.empty()) {
                auto cached = dialogues.find(host.dlg);
                if (cached == dialogues.end()) {
                    kotor::Dlg parsed;
                    const bool found = g.loadDialogue(host.dlg, parsed, 0);
                    cached = dialogues.emplace(host.dlg, std::make_pair(found, std::move(parsed))).first;
                }
                if (!cached->second.first) {
                    out.hostMissing = true;
                    out.missing = host.dlg;
                    return out;
                }
                kotor::Dlg dlg = cached->second.second;
                GameSession::ActiveDialogue active;
                active.npcTag = host.ownerTag;
                active.speaker = host.ownerTag;
                active.resref = host.dlg;
                active.npcObject = owner;
                active.dlg = std::move(dlg);
                g.activeDialogue_ = std::move(active);
                g.dialogueView_ = {};
                g.dialogueView_.active = true;
                ++g.dialogueGeneration_;
                g.sim_->dialoguePaused = false;
                if (host.kind == "dlg_cond") {
                    (void)g.dialogueLinkPasses(host.script, {}); // Its writes are the edge; its answer is not.
                    return out;
                }
                if (!host.linkActive.empty()) {
                    out.condition = g.dialogueLinkPasses(host.linkActive, {});
                    if (!g.activeDialogue_.has_value()) {
                        return out;
                    }
                }
                // One node alone. Walking on would run the following nodes too, and a later
                // script overwriting this one's write would read as a missing effect.
                if (host.kind == "dlg_entry") {
                    const kotor::DlgEntry entry = g.activeDialogue_->dlg.entries.at(host.node);
                    g.applyDialogueNode(entry, g.dialogueGeneration_);
                } else if (host.kind == "dlg_reply") {
                    const kotor::DlgReply reply = g.activeDialogue_->dlg.replies.at(host.node);
                    g.applyDialogueNode(reply, g.dialogueGeneration_);
                } else {
                    g.finishDialogue(host.kind == "dlg_abort");
                }
                return out;
            }
            ScriptContext context = extra;
            if (host.kind == "scene_hook") {
                g.runSceneHook(host.hook, pc, &context);
                return out;
            }
            if (host.kind == "hook") {
                const std::size_t index = ScriptApi{g}.entityFor(owner);
                if (index != static_cast<std::size_t>(-1)) {
                    const auto hook = g.scene_.entities[index].scripts.find(host.hook);
                    if (hook != g.scene_.entities[index].scripts.end() && lowered(hook->second) == host.script) {
                        g.runHook(host.hook, index, pc, &context);
                        return out;
                    }
                }
            }
            // A script with no placed hook, or a template owner: run it on the owner, with
            // the event context a hook would supply.
            ScriptContext hookContext = g.hookContext(host.hook, pc);
            for (const auto& [name, value] : context) {
                hookContext[name] = value;
            }
            g.runScriptWith(host.script, owner, hookContext);
        } catch (const std::exception& exception) {
            out.threw = true;
            out.error = exception.what();
        }
        return out;
    }

    // Deferred work: events, DelayCommand timers and action queues. No frames run, no
    // triggers fire and the player stays put; only what the script asked for proceeds.
    bool idle() const
    {
        if (!g.scriptWorld_.timers.empty() || !g.scriptWorld_.actionQueue.empty() || !g.sim_->events.empty()) {
            return false;
        }
        return std::all_of(g.sim_->actors.begin(), g.sim_->actors.end(),
                           [](const auto& actor) { return actor.second.queue.empty(); });
    }

    template <typename Done>
    void pump(Done done)
    {
        float simulated = 0.0f;
        // A search runs a host many times: after its first run, a short wait sees most of what
        // a DelayCommand does, and every effect it finds is still seen for real.
        if (search != nullptr && !search->plan.empty()) {
            if (!done() && !idle()) {
                g.flushScriptEvents();
                g.updateTimers(6.0f);
                tickActors(ScriptApi{g}, 0.1f);
                g.flushScriptEvents();
            }
            return;
        }
        for (int tick = 0; tick < 200 && simulated < 30.0f; ++tick) {
            if (done()) {
                return;
            }
            if (idle()) {
                return;
            }
            const bool queued = std::any_of(g.sim_->actors.begin(), g.sim_->actors.end(),
                                            [](const auto& actor) { return !actor.second.queue.empty(); });
            if (simulated > 6.0f && g.scriptWorld_.timers.empty()) {
                return;
            }
            const float dt = queued ? 0.1f : 0.5f;
            simulated += dt;
            g.flushScriptEvents();
            g.updateTimers(dt);
            tickActors(ScriptApi{g}, dt);
        }
    }

    // ---- effects ------------------------------------------------------------------------

    struct Before {
        std::map<std::string, std::int64_t> globals;
        std::int32_t xp = 0;
        std::map<std::pair<std::uint32_t, std::string>, std::size_t> items;
        std::int64_t pazaakCards = 0;
    };

    // CItemRepository::AddItem 0055d330: a pazaak card (base item 86) given to the party is a PARTYTABLE
    // count, never an inventory item.
    bool isPazaakCard(const std::string& resref)
    {
        const kotor::ItemTemplate* templ = g.upgradeTemplate(resref);
        return templ != nullptr && templ->baseItem == 86;
    }
    std::int64_t pazaakCardTotal() const
    {
        std::int64_t total = 0;
        for (const std::int32_t count : g.party_.pazaakCards) {
            total += count;
        }
        return total;
    }

    // The count of one template, because the same script may take another item away.
    std::size_t itemCount(std::uint32_t holder, const std::string& resref)
    {
        std::size_t total = 0;
        // A script may equip what it made in the same run; worn is still held.
        std::vector<std::uint32_t> items = g.itemsOf(holder);
        const std::vector<std::uint32_t> worn = g.itemsWornBy(holder);
        items.insert(items.end(), worn.begin(), worn.end());
        for (const std::uint32_t item : items) {
            if (const script::GameObject* object = g.scriptWorld_.get(item); object != nullptr) {
                const auto made = object->attrs.find("item_resref");
                const std::string* held = made != object->attrs.end() ? std::get_if<std::string>(&made->second) : nullptr;
                if (held == nullptr || lowered(*held).substr(0, 16) != lowered(resref).substr(0, 16)) {
                    continue;
                }
                const auto stack = object->attrs.find("item_stack");
                const std::int64_t* count = stack != object->attrs.end() ? std::get_if<std::int64_t>(&stack->second)
                                                                         : nullptr;
                total += count != nullptr ? static_cast<std::size_t>(std::max<std::int64_t>(1, *count)) : 1;
            }
        }
        return total;
    }

    // Makes the effect observable: the variable starts away from the value the script
    // writes, unless a precondition names that same variable.
    void presetFor(const Effect& effect, const Established& established)
    {
        if (effect.kind == "global_set" && !established.touched.contains("global|" + lowered(effect.name))) {
            if (effect.aux.rfind("str", 0) == 0) {
                return;
            }
            // An unknown value may well equal the old one, so that start must be one no script writes.
            constexpr std::int64_t kNoScriptWrites = 64; // A single high bit: `x | 1` must change it.
            const std::int64_t away = effect.valueKnown ? (effect.value == 0 ? 1 : effect.value == 1 ? 0 : effect.value - 1)
                                                        : kNoScriptWrites;
            g.scriptWorld_.setGlobal(effect.name, script::Value::makeInt(away));
        } else if (effect.kind == "journal_remove" && !established.touched.contains("journal|" + lowered(effect.name))) {
            g.addJournalEntry(effect.name, 1);
        }
    }

    bool effectHeld(const Effect& effect, std::uint32_t owner, const Before& before)
    {
        if (effect.kind == "journal_set") {
            if (!effect.valueKnown) {
                return g.journalState(effect.name) != 0;
            }
            return g.journalState(effect.name) == std::abs(static_cast<std::int32_t>(effect.value));
        }
        if (effect.kind == "journal_remove") {
            return g.journalState(effect.name) == 0;
        }
        if (effect.kind == "global_set") {
            if (effect.aux.rfind("str:", 0) == 0) {
                return g.scriptWorld_.getGlobal(effect.name, script::ValueType::String).text == effect.aux.substr(4);
            }
            if (effect.aux == "str") {
                return !g.scriptWorld_.getGlobal(effect.name, script::ValueType::String).text.empty();
            }
            const std::int64_t now = g.scriptWorld_.getGlobal(effect.name, script::ValueType::Int).integer;
            if (effect.valueKnown) {
                if (now == effect.value) {
                    return true;
                }
                // Written, then overwritten later in the same run: the store still has
                // to hold that later write for this to count.
                const std::string name = lowered(effect.name);
                std::optional<std::int64_t> last;
                bool wrote = false;
                for (const auto& [written, value] : globalWrites) {
                    if (written == name) {
                        last = value;
                        wrote = wrote || value == effect.value;
                    }
                }
                return wrote && last == now;
            }
            const auto was = before.globals.find(effect.name);
            return was == before.globals.end() || was->second != now;
        }
        if (effect.kind == "xp" || effect.kind == "plot_xp") {
            if (effect.valueKnown && effect.name.rfind("plotindex:", 0) == 0
                && g.plotXpForIndex(static_cast<std::int32_t>(effect.value), std::stof(effect.aux)) == 0) {
                return true; // This node's share of the plot.2da row is nothing.
            }
            if (effect.kind == "plot_xp" && effect.valueKnown && effect.name.rfind("plotindex:", 0) != 0
                && g.plotXpByLabel(effect.name, static_cast<double>(effect.value) * 0.01) == 0) {
                return true; // Rounds down to nothing.
            }
            return g.character_.xp > before.xp;
        }
        if (effect.kind == "item_create") {
            const std::uint32_t holder = resolveOwner(effect.aux, owner);
            if (holder != script::kObjectInvalid && canHoldItems(holder) && isPazaakCard(effect.name)
                && pazaakCardTotal() > before.pazaakCards) {
                return true;
            }
            const auto was = before.items.find({holder, effect.name});
            return holder != script::kObjectInvalid && was != before.items.end()
                && itemCount(holder, effect.name) > was->second;
        }
        if (effect.kind == "start_module") {
            return lowered(g.pendingModule_) == lowered(effect.name);
        }
        if (effect.kind == "end_game") {
            return g.sim_->ending.pending || g.sim_->ending.ended;
        }
        return false;
    }

    // XP a journal entry's first arrival should award, from global.jrl and plot.2da.
    std::int32_t journalXpDue(const Effect& effect)
    {
        const JournalCategory* category = g.journalCategory(effect.name);
        if (category == nullptr || !effect.valueKnown) {
            return 0;
        }
        const auto entry = category->entries.find(std::abs(static_cast<std::int32_t>(effect.value)));
        if (entry == category->entries.end()) {
            return 0;
        }
        return g.plotXp(category->plotIndex, entry->second.xpPercentage);
    }

    bool canHoldItems(std::uint32_t object)
    {
        if (object == g.scriptWorld_.pc) {
            return true;
        }
        const std::size_t index = ScriptApi{g}.entityFor(object);
        if (index == static_cast<std::size_t>(-1)) {
            return false;
        }
        const kotor::EntityKind kind = g.scene_.entities[index].kind;
        return kind == kotor::EntityKind::Creature || kind == kotor::EntityKind::Placeable
            || kind == kotor::EntityKind::Store;
    }

    bool scriptMissing(const std::string& resref) const
    {
        return !resref.empty() && g.missingScripts_.contains(resref);
    }

    std::string vmProblem() const
    {
        for (const std::string& line : g.scriptLog_) {
            if (line.find(" status=error") != std::string::npos || line.find(" status=budget") != std::string::npos) {
                const std::size_t error = line.find(" error=");
                return error != std::string::npos ? line.substr(0, line.find(' ', 8)) + line.substr(error, 120)
                                                  : line.substr(0, 120);
            }
        }
        return {};
    }

    // The global this edge's write or gate rests on, if retail drops it.
    // Retail keeps a global only under its catalog row's kind (00529030): no row at all, or a row of
    // the other kind, and the read gives 0 and the write does nothing.
    std::string droppedClass(const std::string& name) const
    {
        const bool otherKind = g.runtime_->globalDeclared(name, script::GlobalKind::Boolean)
            || g.runtime_->globalDeclared(name, script::GlobalKind::Number);
        return (otherKind ? "global_wrong_kind:" : "global_undeclared:") + name;
    }

    std::string droppedFor(const Edge& edge, const ReqSet& reqs) const
    {
        if (edge.effect.kind == "global_set" && droppedGlobals.contains(lowered(edge.effect.name))) {
            return lowered(edge.effect.name);
        }
        for (const Req& req : reqs) {
            if (req.kind == "global" && droppedGlobals.contains(lowered(req.name))) {
                return lowered(req.name);
            }
        }
        return {};
    }

    // ---- one edge group -----------------------------------------------------------------

    void runGroup(const std::vector<const Edge*>& group, std::size_t alternative,
                  std::map<std::string, EdgeResult>& results)
    {
        const Edge& lead = *group.front();
        using clock = std::chrono::steady_clock;
        const auto seconds = [](clock::time_point since) {
            return std::chrono::duration<double>(clock::now() - since).count();
        };
        const auto restoreStart = clock::now();
        restore();
        timeRestore += seconds(restoreStart);
        bool fallback = false;
        std::uint32_t owner = ownerFor(lead.host, fallback);
        if (ownerOverride != 0) {
            owner = ownerOverride;
            fallback = false;
        }
        Established established;
        const ReqSet& reqs = alternative < lead.gates.size() ? lead.gates[alternative] : ReqSet{};
        // GetFirstPC() == OBJECT_SELF is no query to answer: the script runs on the PC, for real.
        if (std::any_of(reqs.begin(), reqs.end(), [](const Req& req) {
                return req.name == "pc~self" && (req.op == "eq" || req.op == "true");
            })) {
            owner = g.scriptWorld_.pc;
            fallback = false;
        }
        establish(reqs, owner, established);
        for (const Edge* edge : group) {
            presetFor(edge->effect, established);
        }
        Before before;
        before.xp = g.character_.xp;
        before.pazaakCards = pazaakCardTotal();
        for (const Edge* edge : group) {
            if (edge->effect.kind == "global_set" && edge->effect.aux.rfind("str", 0) != 0) {
                before.globals[edge->effect.name] =
                    g.scriptWorld_.getGlobal(edge->effect.name, script::ValueType::Int).integer;
            }
            if (edge->effect.kind == "item_create") {
                const std::uint32_t holder = resolveOwner(edge->effect.aux, owner);
                if (holder != script::kObjectInvalid) {
                    before.items[{holder, edge->effect.name}] = itemCount(holder, edge->effect.name);
                }
            }
        }
        // The same effect from another line of the script does not prove this one ran.
        std::unordered_set<std::uint64_t> ownCoverage;
        struct CoverageScope {
            std::unordered_set<std::uint64_t>* used;
            bool mine;
            explicit CoverageScope(std::unordered_set<std::uint64_t>& own) : used(script::Vm::coverage), mine(used == nullptr)
            {
                if (mine) {
                    used = &own;
                    script::Vm::coverage = &own;
                }
            }
            ~CoverageScope()
            {
                if (mine) {
                    script::Vm::coverage = nullptr;
                }
            }
            CoverageScope(const CoverageScope&) = delete;
            CoverageScope& operator=(const CoverageScope&) = delete;
        } coverageScope(ownCoverage);
        const auto runStart = clock::now();
        const RunOutcome outcome = runHost(lead.host, owner, established.context);
        timeRun += seconds(runStart);
        const auto pumpStart = clock::now();
        const auto allHeld = [&]() {
            return std::all_of(group.begin(), group.end(),
                               [&](const Edge* edge) { return effectHeld(edge->effect, owner, before); });
        };
        pump(allHeld);
        // PlayPazaak keeps its end script until a match ends, and no match is played here. The
        // script runs as EndPazaakGame 005f3950 runs it (RunScript(script, 0x7f000000): no
        // OBJECT_SELF, no event objects), with GetLastPazaakResult answered by what its branch needs.
        if (!g.pazaakEndScript_.empty()) {
            const std::string endScript = g.pazaakEndScript_;
            g.stopPazaak();
            g.runScript(endScript, script::kObjectInvalid);
            pump(allHeld);
        }
        // A party screen the host opened, often from a DelayCommand, closes as the player would
        // leave it, which runs its exit script.
        if (g.partySelection_.active) {
            g.cancelPartySelection();
            pump(allHeld);
        }
        timePump += seconds(pumpStart);
        const std::string problem = vmProblem();
        for (const Edge* edge : group) {
            EdgeResult& result = results[edge->id];
            if (result.status == Status::Pass) {
                continue;
            }
            if (effectHeld(edge->effect, owner, before) && edge->siteOffset != 0
                && !coverageScope.used->contains(script::Vm::coverageKey(edge->siteScript.empty() ? edge->host.script : edge->siteScript, edge->siteOffset))) {
                result = {Status::Cant, "effect_from_other_site"};
                continue;
            }
            if (effectHeld(edge->effect, owner, before)) {
                const std::int32_t due = edge->effect.kind == "journal_set" ? journalXpDue(edge->effect) : 0;
                if (due > 0 && before.xp + due > g.character_.xp) {
                    result = {Status::Fail, "xp_missing:" + std::to_string(due)};
                } else if (outcome.condition.has_value() && !*outcome.condition) {
                    // A condition the graph could not model, or one resting on an answered
                    // query, says nothing about the engine.
                    if (!established.opaque.empty()) {
                        result = {Status::Cant, "condition_false_opaque:" + established.opaque.front()};
                    } else if (!established.mocked.empty()) {
                        result = {Status::Cant, "condition_false_mock:" + established.mocked.front()};
                    } else if (!established.unsatisfied.empty()) {
                        // The condition and the script's own branch want different values.
                        result = {Status::Cant, "condition_vs_branch:" + established.unsatisfied.front()};
                    } else if (!droppedGlobals.empty()) {
                        // The condition reads a global its catalog row does not allow (00529030).
                        result = {Status::Dead, droppedClass(*droppedGlobals.begin())};
                    } else if (reqs.empty()) {
                        result = {Status::Cant, "condition_not_modelled:" + edge->host.linkActive};
                    } else {
                        result = {Status::Fail, "condition_false:" + edge->host.linkActive};
                    }
                } else if (!established.mocked.empty()) {
                    result = {Status::Pass, "mock:" + established.mocked.front()};
                } else {
                    result = {Status::Pass, {}};
                }
                continue;
            }
            if (outcome.threw) {
                result = {Status::Fail, "exception:" + outcome.error.substr(0, 80)};
            } else if (outcome.hostMissing) {
                result = {Status::Cant, "dlg_not_found:" + outcome.missing};
            } else if (!problem.empty() && established.mocked.empty()) {
                result = {Status::Fail, "vm:" + problem};
            } else if (scriptMissing(edge->host.script) || scriptMissing(edge->siteScript)) {
                result = {Status::Cant, "script_not_shipped"};
            } else if (fallback) {
                result = {Status::Cant, "owner_not_available"};
            } else if (!established.opaque.empty()) {
                result = {Status::Cant, "opaque:" + established.opaque.front()};
            } else if (!established.mocked.empty()) {
                result = {Status::Cant, "effect_missing_mock:" + established.mocked.front()};
            } else if (!established.unsatisfied.empty()) {
                const std::string& what = established.unsatisfied.front();
                result = what.rfind("infeasible|", 0) == 0 ? EdgeResult{Status::Cant, "infeasible:" + what.substr(11)}
                                                           : EdgeResult{Status::Cant, "unsatisfied:" + what};
            } else if (edge->effect.kind == "item_create" && edge->effect.nameKnown
                       && !kotor::loadItemTemplate(g.install_, lowered(edge->effect.name).substr(0, 16))) {
                // retail CreateItemOnObject 0052f570: a template that does not load makes no item,
                // and only this module and the global archives are open.
                const auto home = graph.itemTemplateHomes.find(lowered(edge->effect.name));
                result = home == graph.itemTemplateHomes.end()
                    ? EdgeResult{Status::Dead, "template_not_shipped:" + lowered(edge->effect.name)}
                    : EdgeResult{Status::Dead, "template_only_in:" + home->second.front() + ":" + lowered(edge->effect.name)};
            } else if (edge->effect.kind == "item_create" && !canHoldItems(resolveOwner(edge->effect.aux, owner))) {
                // Retail destroys an item with nowhere to go (0052f570). The graph's owner is
                // only a guess when the script names OBJECT_SELF.
                result = {Status::Cant, "holder_cannot_hold"};
            } else if (const std::string droppedHere = droppedFor(*edge, reqs); !droppedHere.empty()) {
                result = {Status::Dead, droppedClass(droppedHere)};
            } else if (edge->orphan) {
                result = {Status::Cant, "orphan_no_context"};
            } else if (edge->debug) {
                result = {Status::Cant, "debug_script"};
            } else if (!edge->effect.valueKnown && established.touched.contains("global|" + lowered(edge->effect.name))) {
                result = {Status::Cant, "unknown_value_unchanged"};
            } else {
                result = {Status::Fail, "effect_missing"};
            }
            if (result.status == Status::Fail && failLog != nullptr) {
                if (edge->effect.kind == "item_create") {
                    // Why the item is not there: try the same call again now the run is over.
                    const std::uint32_t holder = resolveOwner(edge->effect.aux, owner);
                    const std::size_t entity = ScriptApi{g}.entityFor(holder);
                    const std::uint32_t again = g.giveItem(lowered(edge->effect.name), holder, 1);
                    *failLog << "    holder=" << holder << " owner=" << owner << " pc=" << g.scriptWorld_.pc
                             << " entity_tag="
                             << (entity != static_cast<std::size_t>(-1) ? g.scene_.entities[entity].tag : std::string("none"))
                             << " again=" << again << " count_after=" << itemCount(holder, edge->effect.name) << '\n';
                }
                *failLog << edge->id << '\t' << result.reason << '\n';
                for (const std::string& line : g.scriptLog_) {
                    *failLog << "    " << line.substr(0, 400) << '\n';
                }
            }
        }
    }

    // ---- directed search -------------------------------------------------------------------

    // A function of its arguments alone. Its answer is never invented: if it comes out wrong
    // for a path, what has to change is the state its argument came from.
    static bool pureQuery(std::string_view name)
    {
        static const std::set<std::string_view> names{
            "FindSubString", "GetStringLength", "GetStringLeft", "GetStringRight", "GetSubString",
            "GetStringUpperCase", "GetStringLowerCase", "InsertString", "IntToString", "IntToFloat",
            "IntToHexString", "FloatToInt", "FloatToString", "StringToInt", "StringToFloat", "abs", "fabs",
            "cos", "sin", "tan", "acos", "asin", "atan", "log", "pow", "sqrt", "Vector", "VectorMagnitude",
            "VectorNormalize", "VectorToAngle", "AngleToVector", "ShipBuild", "RoundsToSeconds",
            "HoursToSeconds", "TurnsToSeconds", "FeetToMeters", "YardsToMeters",
        };
        return names.contains(name);
    }

    // Queries a search may answer: what the world could have said, in the range the action allows.
    static bool searchable(const script::ActionSpec& spec)
    {
        const std::string_view name(spec.name);
        if (name == "GetFirstPC") {
            return true; // retail 0053c920 asks the party for its leader: a companion can be it.
        }
        if (pureQuery(name) || name == "GetIsObjectValid" || name == "GetWaypointByTag" || name == "GetRunScriptVar" || name == "GetModule"
            || name == "GetModuleFileName" || name == "GetPCSpeaker" || name == "GetArea") {
            return false;
        }
        const bool queryName = name.rfind("Get", 0) == 0 || name.rfind("Is", 0) == 0 || name == "Random"
            || (name.size() >= 2 && name[0] == 'd' && std::isdigit(static_cast<unsigned char>(name[1])) != 0);
        return queryName
            && (spec.returnKind == script::ReturnKind::Int || spec.returnKind == script::ReturnKind::String
                || spec.returnKind == script::ReturnKind::Object);
    }

    static bool walkableQuery(std::string_view asked)
    {
        return asked == "Random" || asked == "IsNPCPartyMember" || asked == "GetHitDice"
            || (asked.size() >= 2 && asked[0] == 'd' && std::isdigit(static_cast<unsigned char>(asked[1])) != 0);
    }

    struct SearchQuery {
        std::uint16_t id = 0;
        int nth = 0;
        std::vector<script::Value> candidates;
    };
    using SearchPlan = std::map<std::pair<std::uint16_t, int>, script::Value>;
    struct SearchRun {
        SearchPlan plan;
        std::map<std::uint16_t, int> counts;
        std::vector<SearchQuery> seen;
        std::vector<SearchQuery> walkable; // Random, dice, party and level queries, however early they came.
        std::set<std::string> gone;        // Tags the path wants invalid: destroyed when first looked up.
        std::set<std::string> answered; // Queries given a value no state write stands behind.
        bool leader = false;            // The host may be the controlled character.
        std::map<std::string, std::int32_t> presets; // Globals written on first read, from the code before the site.
    };
    SearchRun* search = nullptr;

    // Constants the code tests a query's result against: the values worth trying.
    static void nearbyConstants(const script::Vm& vm, std::set<std::int64_t>& ints, std::set<std::string>& strings)
    {
        const auto& code = vm.code();
        const std::uint32_t offset = vm.currentOffset();
        const auto at = std::lower_bound(code.begin(), code.end(), offset,
                                         [](const script::Instruction& ins, std::uint32_t wanted) { return ins.offset < wanted; });
        std::size_t scanned = 0;
        for (auto it = at; it != code.end() && scanned < 14; ++it, ++scanned) {
            if (it->opcode == 0x04 && it->qualifier == 0x03) {
                ints.insert({std::int64_t{it->operand0}, std::int64_t{it->operand0} - 1, std::int64_t{it->operand0} + 1});
            } else if (it->opcode == 0x04 && it->qualifier == 0x05) {
                strings.emplace(it->bytes.begin(), it->bytes.end());
            }
        }
    }

    std::vector<script::Value> candidatesFor(ActionCall& call, const script::ActionSpec& spec,
                                             const std::optional<std::vector<script::Value>>& real)
    {
        std::vector<script::Value> out;
        const std::string_view name(spec.name);
        std::set<std::int64_t> ints{0, 1};
        std::set<std::string> strings{std::string{}};
        nearbyConstants(call.vm, ints, strings);
        if (spec.returnKind == script::ReturnKind::Int) {
            std::int64_t low = std::numeric_limits<std::int32_t>::min();
            std::int64_t high = std::numeric_limits<std::int32_t>::max();
            if (name == "Random") {
                low = 0;
                high = std::max<std::int64_t>(0, call.intArg(0) - 1);
                ints.insert(high);
            } else if (name[0] == 'd') {
                const std::int64_t dice = std::max<std::int64_t>(1, call.intArg(0, 1));
                low = dice;
                high = dice * std::max<std::int64_t>(2, std::atoll(std::string(name.substr(1)).c_str()));
                ints.insert({low, high});
            }
            if (name == "Random" && high < 128) {
                for (std::int64_t value = low; value <= high; ++value) {
                    ints.insert(value);
                }
            }
            for (const std::int64_t value : ints) {
                if (value >= low && value <= high && out.size() < (name == "Random" ? 128U : 10U)) {
                    out.push_back(script::Value::makeInt(static_cast<std::int32_t>(value)));
                }
            }
        } else if (spec.returnKind == script::ReturnKind::String) {
            for (const std::string& value : strings) {
                out.push_back(script::Value::makeString(value));
            }
        } else if (spec.returnKind == script::ReturnKind::Object) {
            const bool realValid = real.has_value() && !real->empty() && static_cast<std::uint32_t>(real->front().integer) != script::kObjectInvalid;
            if (name == "GetObjectByTag" || name == "GetItemPossessedBy") {
                if (!realValid) {
                    out.push_back(script::Value::makeInt(1)); // Bring the real object into being first.
                }
            } else if (name == "GetFirstPC") {
                // Not by the call: see the leader phase of sweepDirected.
            } else if (realValid) {
                out.push_back(script::Value::makeObject(script::kObjectInvalid));
                out.push_back(script::Value::makeObject(g.scriptWorld_.pc));
            } else {
                out.push_back(script::Value::makeObject(g.scriptWorld_.pc));
                out.push_back(script::Value::makeObject(call.self()));
            }
        }
        return out;
    }

    std::optional<std::vector<script::Value>> searchAction(ActionCall& call)
    {
        const script::ActionSpec& spec = script::actionSpec(call.id);
        const std::string_view name(spec.name);
        // With the host as the controlled character, every ask of the party for its leader says so.
        if (search->leader && name == "GetFirstPC" && call.self() != g.scriptWorld_.pc) {
            search->answered.insert(std::string(name));
            return std::vector<script::Value>{script::Value::makeObject(call.self())};
        }
        if (name == "GetObjectByTag" && !search->gone.empty()) {
            const std::string tag = lowered(call.stringArg(0));
            if (search->gone.erase(tag) != 0) {
                const std::uint32_t object = g.scriptWorld_.byTag(tag, 0);
                if (script::GameObject* held = g.scriptWorld_.get(object); held != nullptr) {
                    held->valid = false;
                    const std::size_t entity = ScriptApi{g}.entityFor(object);
                    if (entity != static_cast<std::size_t>(-1)) {
                        g.scene_.entities[entity].dead = true;
                    }
                    search->answered.insert("gone:" + tag);
                }
            }
        }
        if ((call.id == kGetGlobalBoolean || call.id == kGetGlobalNumber) && !search->presets.empty()) {
            const auto preset = search->presets.find(lowered(call.stringArg(0)));
            if (preset != search->presets.end()) {
                g.scriptWorld_.setGlobal(call.stringArg(0), script::Value::makeInt(preset->second));
                search->answered.insert("preset:" + preset->first);
                search->presets.erase(preset);
            }
        }
        const int nth = search->counts[call.id]++;
        const auto planned = search->plan.find({call.id, nth});
        const bool isPlanned = planned != search->plan.end();
        if (isPlanned) {
            const script::Value& value = planned->second;
            // A read of real state: write the state and let the real handler read it.
            if (call.id == kGetGlobalBoolean || call.id == kGetGlobalNumber) {
                g.scriptWorld_.setGlobal(call.stringArg(0), script::Value::makeInt(value.asInt()));
            } else if (name == "GetLocalBoolean" || name == "GetLocalNumber") {
                const std::vector<std::vector<script::Value>> args{{script::Value::makeObject(call.objectArg(0))},
                                                                   {script::Value::makeInt(static_cast<std::int32_t>(call.intArg(1)))},
                                                                   {script::Value::makeInt(value.asInt())}};
                g.actionAsForTest(call.self(), name == "GetLocalBoolean" ? kSetLocalBoolean : kSetLocalNumber, args);
            } else if (name == "GetJournalEntry") {
                g.addJournalEntry(call.stringArg(0), static_cast<std::int32_t>(value.asInt()));
            } else if (name == "GetObjectByTag") {
                spawnForTag(call.stringArg(0));
            } else if (name == "GetItemPossessedBy") {
                g.giveItem(lowered(call.stringArg(1)), call.objectArg(0), 1);
            } else {
                search->answered.insert(std::string(name));
                return std::vector<script::Value>{value};
            }
        }
        const ActionHandler original = originals[call.id];
        std::optional<std::vector<script::Value>> real;
        if (name == "Random") {
            real = std::vector<script::Value>{script::Value::makeInt(0)}; // A value of the search's own, so runs repeat.
        } else if (name.size() >= 2 && name[0] == 'd' && std::isdigit(static_cast<unsigned char>(name[1])) != 0) {
            real = std::vector<script::Value>{script::Value::makeInt(static_cast<std::int32_t>(std::max<std::int64_t>(1, call.intArg(0, 1))))};
        } else if (original != nullptr) {
            real = original(call);
        }
        if (!isPlanned) {
            // The latest queries are the ones nearest where a run stopped: keep those.
            if (search->seen.size() >= 64) {
                search->seen.erase(search->seen.begin() + 32); // The first reads are the script's own prologue.
            }
            SearchQuery query;
            query.id = call.id;
            query.nth = nth;
            query.candidates = candidatesFor(call, spec, real);
            if (real.has_value() && real->size() == 1 && spec.returnKind == script::ReturnKind::Int) {
                const std::int64_t said = real->front().asInt();
                std::erase_if(query.candidates, [&](const script::Value& value) { return value.asInt() == said; });
            }
            if (!query.candidates.empty()) {
                if (walkableQuery(name) && search->walkable.size() < 256) {
                    search->walkable.push_back(query);
                }
                search->seen.push_back(std::move(query));
            }
        }
        return real;
    }

    static std::string planKey(std::size_t lead, std::size_t alternative, const SearchPlan& plan)
    {
        std::string key = std::to_string(lead) + "/" + std::to_string(alternative);
        for (const auto& [where, value] : plan) {
            key += "|" + std::to_string(where.first) + "." + std::to_string(where.second) + "=" + std::to_string(value.asInt()) + value.asString();
        }
        return key;
    }

    // The branch a run kept missing: taken in some run, with the side that leads to the site never
    // taken. Names the script, the offset and the last query before it.
    std::string blockerFor(const std::string& script, std::uint32_t siteOffset, const std::unordered_set<std::uint64_t>& covered)
    {
        const auto program = g.decodedScript(script);
        if (program == nullptr) {
            return "script_not_shipped:" + script;
        }
        const std::vector<script::Instruction>& code = program->code;
        std::unordered_map<std::uint32_t, std::size_t> index;
        for (std::size_t i = 0; i < code.size(); ++i) {
            index[code[i].offset] = i;
        }
        const auto siteAt = index.find(siteOffset);
        if (siteAt == index.end()) {
            return "site_not_in:" + script;
        }
        const auto jumpOf = [&](const script::Instruction& ins) -> std::optional<std::size_t> {
            const auto it = index.find(static_cast<std::uint32_t>(static_cast<std::int32_t>(ins.offset) + ins.operand0));
            return it == index.end() ? std::nullopt : std::optional<std::size_t>(it->second);
        };
        std::vector<std::vector<std::size_t>> before(code.size());
        for (std::size_t i = 0; i < code.size(); ++i) {
            const script::Op op = code[i].op;
            if (op != script::Op::JMP && op != script::Op::RETN && i + 1 < code.size()) {
                before[i + 1].push_back(i);
            }
            if (op == script::Op::JMP || op == script::Op::JZ || op == script::Op::JNZ || op == script::Op::JSR) {
                if (const auto to = jumpOf(code[i]); to.has_value()) {
                    before[*to].push_back(i);
                }
            }
        }
        std::vector<int> distance(code.size(), -1);
        std::queue<std::size_t> queue;
        distance[siteAt->second] = 0;
        queue.push(siteAt->second);
        while (!queue.empty()) {
            const std::size_t at = queue.front();
            queue.pop();
            for (const std::size_t from : before[at]) {
                if (distance[from] < 0) {
                    distance[from] = distance[at] + 1;
                    queue.push(from);
                }
            }
        }
        const auto ran = [&](std::size_t i) { return covered.contains(script::Vm::coverageKey(script, code[i].offset)); };
        std::optional<std::size_t> best;
        bool anyRan = false;
        for (std::size_t i = 0; i < code.size(); ++i) {
            anyRan = anyRan || ran(i);
            if (!ran(i) || (code[i].op != script::Op::JZ && code[i].op != script::Op::JNZ) || distance[i] < 0) {
                continue;
            }
            const auto to = jumpOf(code[i]);
            const bool nextOpen = i + 1 < code.size() && !ran(i + 1) && distance[i + 1] >= 0;
            const bool jumpOpen = to.has_value() && !ran(*to) && distance[*to] >= 0;
            if ((nextOpen || jumpOpen) && (!best.has_value() || distance[i] < distance[*best])) {
                best = i;
            }
        }
        if (!anyRan) {
            return "not_run:" + script;
        }
        if (!best.has_value()) {
            return ran(siteAt->second) ? "site_reached:" + script + "@" + std::to_string(siteOffset)
                                       : "no_branch:" + script + "@" + std::to_string(siteOffset);
        }
        std::string query = "none";
        for (std::size_t i = *best, seen = 0; i > 0 && seen < 24; --i, ++seen) {
            if (code[i].op == script::Op::ACTION && code[i].actionId < kScriptActionCount) {
                query = script::actionSpec(code[i].actionId).name;
                break;
            }
            if (code[i].op == script::Op::JSR) {
                query = "function_result";
                break;
            }
        }
        return script + "@" + std::to_string(code[*best].offset) + "(" + query + ")";
    }

    // Runs the host again and again, changing the answer to one world query or the state behind
    // one read at a time, and keeps going from any change that ran code no run had reached. Stops
    // when every open effect has been seen on a real run of the shipped code.
    void sweepDirected(const std::vector<const Edge*>& group, std::map<std::string, EdgeResult>& results)
    {
        // The budget counts work, never time, so two runs of the same binary agree edge for edge.
        const int kMostRuns = std::clamp(static_cast<int>(group.size()) * 8, 240, 500);
        const std::uint64_t workStart = workDone();
        constexpr std::size_t kMostSeeds = 6;
        // Four passes over the same seeds. Earliest query first finds what an early answer decides (a
        // skill check behind a party loop); latest first finds what the last branch reached decides (a
        // long library prologue before it). Then both again with the host as the controlled character.
        int phase = 0;
        bool latestFirst = false;
        struct Trial {
            std::size_t lead = 0;
            std::size_t alternative = 0;
            SearchPlan plan;
        };
        std::vector<const Edge*> open = group;
        std::deque<Trial> queue;
        std::unordered_set<std::uint64_t> tried; // Hashes: a thorough walk would hold thousands of long keys.
        std::set<std::string> seedKeys;
        for (std::size_t i = 0; i < group.size() && queue.size() < kMostSeeds; ++i) {
            for (std::size_t alternative = 0; alternative < std::max<std::size_t>(1, group[i]->gates.size()) && alternative < 3; ++alternative) {
                std::string key;
                if (alternative < group[i]->gates.size()) {
                    for (const Req& req : group[i]->gates[alternative]) {
                        key += req.key() + ";";
                    }
                }
                if (seedKeys.insert(key).second && queue.size() < kMostSeeds) {
                    queue.push_back({i, alternative, {}});
                }
            }
        }
        // What worked for the same site and effect in another module is tried first.
        for (std::size_t i = group.size(); i-- > 0;) {
            if (const auto known = plansThatWorked.find(siteKey(*group[i])); known != plansThatWorked.end()) {
                queue.push_front({i, 0, known->second});
            }
        }
        std::unordered_set<std::uint64_t> covered;
        std::unordered_set<std::uint64_t> thisRun;
        Variant real;
        real.allReal = true;
        int runs = 0;
        int runLimit = kMostRuns;
        std::uint64_t workLimit = kSearchWork;
        const std::deque<Trial> seeds = queue;
        bool leader = false;
        bool enumerable = false; // A run asked Random or who is in the party: those values can all be walked.
        bool thorough = false;
        std::uint64_t thoroughStart = 0;
        const auto scriptsOfOpen = [&]() {
            std::set<std::string> scripts;
            for (const Edge* edge : open) {
                scripts.insert(edge->siteScript.empty() ? edge->host.script : edge->siteScript);
            }
            return scripts;
        };
        while (!open.empty()) {
            if (queue.empty() || runs >= runLimit || workDone() - workStart >= workLimit) {
                ++phase;
                const std::set<std::string> openScripts = scriptsOfOpen();
                const bool walkedAlready = std::all_of(openScripts.begin(), openScripts.end(), [&](const std::string& script) {
                    return thoroughFailed.contains(script);
                });
                if (phase == 4 && enumerable && !walkedAlready && thoroughWork < kAllThoroughWork && openScriptsAsk(openScripts)) {
                    // Every value of the enumerable queries, fewest changed answers first.
                    thorough = true;
                    latestFirst = true;
                    leader = false;
                    runLimit = kThoroughRuns;
                    thoroughStart = workDone();
                    workLimit = workDone() - workStart + kThoroughWork;
                } else if (phase >= 4) {
                    break;
                } else {
                    leader = phase >= 2;
                    latestFirst = phase % 2 == 1;
                    if (workDone() - workStart >= workLimit) {
                        workLimit = workDone() - workStart + kSearchWork / 4;
                    }
                }
                queue = seeds;
                if (thorough) {
                    queue.clear();
                    std::set<std::string> ways;
                    for (std::size_t i = 0; i < group.size(); ++i) {
                        if (std::find(open.begin(), open.end(), group[i]) == open.end()) {
                            continue;
                        }
                        std::string way;
                        for (const auto& [name, value] : guardsBeforeSite(*group[i])) {
                            way += name + "=" + std::to_string(value) + ";";
                        }
                        if (!group[i]->gates.empty()) {
                            for (const Req& req : group[i]->gates.front()) {
                                if (req.kind == "exists") {
                                    way += req.key() + ";";
                                }
                            }
                        }
                        if (ways.insert(way).second) {
                            queue.push_back({i, 0, {}});
                        }
                    }
                }
                tried.clear();
                covered.clear();
                runs = 0;
            }
            Trial trial = std::move(queue.front());
            queue.pop_front();
            ++runs;
            SearchRun run;
            run.leader = leader;
            run.plan = trial.plan;
            if (leader || thorough) {
                run.presets = guardsBeforeSite(*group[trial.lead]);
                if (!group[trial.lead]->gates.empty()) {
                    for (const Req& req : group[trial.lead]->gates.front()) {
                        if (req.kind == "exists" && (req.op == "false" || (req.op == "eq" && req.value == 0))) {
                            run.gone.insert(lowered(req.name));
                        }
                    }
                }
            }
            thisRun.clear();
            std::vector<const Edge*> ordered{group[trial.lead]};
            for (const Edge* edge : open) {
                if (edge != group[trial.lead]) {
                    ordered.push_back(edge);
                }
            }
            search = &run;
            variant = &real;
            script::Vm::coverage = &thisRun;
            std::map<std::string, EdgeResult> scratch;
            const std::size_t peakBefore = peakMemoryMb();
            runGroup(ordered, trial.alternative, scratch);
            if (peakMemoryMb() > peakBefore + 200 && SDL_getenv("KOTOR_QUEST_DIAG") != nullptr) {
                std::printf("quest trace diag: search run grew peak %zu -> %zuMB host %s plan size %zu phase %d\n", peakBefore, peakMemoryMb(),
                            group[trial.lead]->id.c_str(), trial.plan.size(), phase);
                for (const auto& [where, value] : trial.plan) {
                    std::printf("  %s nth %d = %s\n", script::actionSpec(where.first).name, where.second, value.asString().empty() ? std::to_string(value.asInt()).c_str() : value.asString().c_str());
                }
            }
            script::Vm::coverage = nullptr;
            variant = nullptr;
            search = nullptr;
            ++searchRuns;
            std::vector<const Edge*> still;
            for (const Edge* edge : open) {
                const EdgeResult& seen = scratch[edge->id];
                if (seen.status == Status::Pass) {
                    std::string reason;
                    for (const std::string& name : run.answered) {
                        reason += (reason.empty() ? "mock:search:" : ",") + name;
                    }
                    results[edge->id] = {Status::Pass, reason};
                    if (!run.plan.empty()) {
                        plansThatWorked.emplace(siteKey(*edge), run.plan);
                    }
                } else {
                    still.push_back(edge);
                }
            }
            open = std::move(still);
            enumerable = enumerable || !run.walkable.empty();
            bool freshCode = false;
            for (const std::uint64_t key : thisRun) {
                freshCode = covered.insert(key).second || freshCode;
            }
            // A site that ran with the wrong value (one edge per value it can write) still has
            // answers left to try, though the run reached no new code.
            bool siteRan = false;
            for (const Edge* edge : open) {
                siteRan = siteRan || thisRun.contains(script::Vm::coverageKey(edge->siteScript.empty() ? edge->host.script : edge->siteScript, edge->siteOffset));
            }
            if (!thorough && !freshCode && !siteRan && !trial.plan.empty()) {
                continue;
            }
            // A phase runs at most kMostRuns trials, so nothing queued past that many is ever reached.
            // Keeping it cost 5 GB on one treasure table: 128 Random answers at each of 64 queries.
            const std::size_t kMostQueued = static_cast<std::size_t>(runLimit) * 2;
            const auto expand = [&](const SearchQuery& query) {
                if (thorough) {
                    // Breadth first: one changed answer, then every pair, and so on.
                    for (const script::Value& value : query.candidates) {
                        if (queue.size() >= kMostQueued) {
                            break;
                        }
                        Trial next = trial;
                        next.plan[{query.id, query.nth}] = value;
                        if (tried.insert(std::hash<std::string>{}(planKey(next.lead, next.alternative, next.plan))).second) {
                            queue.push_back(std::move(next));
                        }
                    }
                    return;
                }
                for (const script::Value& value : query.candidates) {
                    const bool toFront = freshCode || !latestFirst;
                    if (!toFront && queue.size() >= kMostQueued) {
                        break;
                    }
                    Trial next = trial;
                    next.plan[{query.id, query.nth}] = value;
                    if (tried.insert(std::hash<std::string>{}(planKey(next.lead, next.alternative, next.plan))).second) {
                        // Another value at a known site waits behind new code, latest first only.
                        if (toFront) {
                            queue.push_front(std::move(next));
                            if (queue.size() > kMostQueued) {
                                queue.pop_back();
                            }
                        } else {
                            queue.push_back(std::move(next));
                        }
                    }
                }
            };
            if (thorough) {
                for (const SearchQuery& query : run.walkable) {
                    expand(query);
                }
            } else if (latestFirst) {
                for (const SearchQuery& query : run.seen) {
                    expand(query);
                }
            } else {
                for (auto query = run.seen.rbegin(); query != run.seen.rend(); ++query) {
                    expand(*query);
                }
            }
        }
        if (thorough) {
            const std::set<std::string> left = scriptsOfOpen();
            thoroughFailed.insert(left.begin(), left.end());
            thoroughWork += workDone() - thoroughStart;
        }
        // Another placed holder of the conversation, which may have the tag the script branches on.
        if (!open.empty() && !group.front()->host.dlg.empty()) {
            const std::string dlg = lowered(group.front()->host.dlg);
            bool unused = false;
            const std::uint32_t first = ownerFor(group.front()->host, unused);
            // Doors open this one from a script (k_pman_airlock12), so the field does not name them; the
            // site's script compares GetTag(OBJECT_SELF) with their tags (k_pman_airlock11 @3660).
            std::set<std::string> taggedInScript;
            const std::string siteScriptName = group.front()->siteScript.empty() ? group.front()->host.script : group.front()->siteScript;
            if (const auto program = g.decodedScript(siteScriptName); program != nullptr) {
                const std::vector<script::Instruction>& code = program->code;
                for (std::size_t i = 1; i + 1 < code.size(); ++i) {
                    if (code[i].opcode == 0x04 && code[i].qualifier == 0x05 && code[i - 1].op == script::Op::ACTION
                        && std::string_view(script::actionSpec(code[i - 1].actionId).name) == "GetTag" && code[i + 1].op == script::Op::EQUAL) {
                        taggedInScript.insert(lowered(std::string(code[i].bytes.begin(), code[i].bytes.end())));
                    }
                }
            }
            for (std::size_t i = 0; i < g.scene_.entities.size() && !open.empty(); ++i) {
                const bool holdsIt = lowered(g.scene_.entities[i].conversation) == dlg || taggedInScript.contains(lowered(g.scene_.entities[i].tag));
                if (g.scene_.entities[i].dead || !holdsIt || g.objectForEntity(i) == first) {
                    continue;
                }
                const std::string holderTag = g.scene_.entities[i].tag;
                ownerOverride = g.objectForEntity(i);
                std::map<std::string, EdgeResult> scratch;
                SearchRun run;
                search = &run;
                variant = &real;
                runGroup(open, 0, scratch);
                search = nullptr;
                variant = nullptr;
                ownerOverride = 0;
                ++searchRuns;
                std::vector<const Edge*> still;
                for (const Edge* edge : open) {
                    if (scratch[edge->id].status == Status::Pass) {
                        results[edge->id] = {Status::Pass, "mock:holder:" + holderTag};
                    } else {
                        still.push_back(edge);
                    }
                }
                open = std::move(still);
            }
        }
        for (const Edge* edge : open) {
            EdgeResult& result = results[edge->id];
            if (result.status == Status::Cant) {
                result.reason += ";at=" + blockerFor(edge->siteScript.empty() ? edge->host.script : edge->siteScript,
                                                     edge->siteOffset, covered);
                result.reason += ";runs=" + std::to_string(runs);
            }
        }
    }

    std::size_t searchRuns = 0;
    std::uint32_t ownerOverride = 0;
    std::set<std::string> thoroughFailed; // Scripts one thorough walk did not settle: once is enough.
    std::uint64_t thoroughWork = 0;
    static constexpr std::uint64_t kAllThoroughWork = 4000000000; // Across a whole trace. Uncalibrated.

    // Some open site's own script asks a query a walk can enumerate.
    bool openScriptsAsk(const std::set<std::string>& scripts)
    {
        return std::any_of(scripts.begin(), scripts.end(), [&](const std::string& name) {
            const auto program = g.decodedScript(name);
            return program != nullptr && std::any_of(program->code.begin(), program->code.end(), [](const script::Instruction& ins) {
                return ins.op == script::Op::ACTION && walkableQuery(script::actionSpec(ins.actionId).name);
            });
        });
    }

    static constexpr int kThoroughRuns = 4096;
    // Work is VM instructions plus a fixed cost for each run's restore of the snapshot.
    static constexpr std::uint64_t kRunCost = 20000;
    static constexpr std::uint64_t kSearchWork = 40000000;
    static constexpr std::uint64_t kThoroughWork = 80000000;
    std::uint64_t workDone() const { return script::Vm::totalSteps + searchRuns * kRunCost; }
    std::map<std::string, SearchPlan> plansThatWorked;
    static std::string siteKey(const Edge& edge)
    {
        return (edge.siteScript.empty() ? edge.host.script : edge.siteScript) + "@" + std::to_string(edge.siteOffset) + "|" + edge.effect.key();
    }

    // The globals the code just before a site tests, with the value that lets it through, by the compiler's
    // shape: a read, an optional comparison with a constant, and the jump. A guide only; the run decides.
    std::map<std::string, std::int32_t> guardsBeforeSite(const Edge& edge)
    {
        std::map<std::string, std::int32_t> out;
        const auto program = g.decodedScript(edge.siteScript.empty() ? edge.host.script : edge.siteScript);
        if (program == nullptr) {
            return out;
        }
        const std::vector<script::Instruction>& code = program->code;
        const auto at = std::find_if(code.begin(), code.end(), [&](const script::Instruction& ins) { return ins.offset == edge.siteOffset; });
        if (at == code.end()) {
            return out;
        }
        const std::size_t site = static_cast<std::size_t>(at - code.begin());
        const std::string written = lowered(edge.effect.name);
        // A site in a helper is guarded by the code before its call too: k_pman_26a_area stores the
        // party in a function it calls only behind MAN_SITHBASE_SEALED. Only a helper with one caller
        // says anything about the way in.
        std::vector<std::size_t> scanFrom{site};
        for (std::size_t scan = 0; scan < scanFrom.size() && scan < 4; ++scan) {
            std::size_t entry = 0;
            for (std::size_t i = scanFrom[scan]; i > 0; --i) {
                if (code[i - 1].op == script::Op::RETN) {
                    entry = i;
                    break;
                }
            }
            std::size_t callers = 0;
            std::size_t caller = 0;
            for (std::size_t i = 0; i < code.size() && entry != 0; ++i) {
                if (code[i].op == script::Op::JSR && code[i].offset + code[i].operand0 == code[entry].offset) {
                    ++callers;
                    caller = i;
                }
            }
            if (callers == 1) {
                scanFrom.push_back(caller);
            }
        }
        for (const std::size_t from : scanFrom)
        for (std::size_t back = 1; back < 128 && back < from; ++back) {
            const std::size_t i = from - back;
            if (code[i].op == script::Op::RETN) {
                break;
            }
            if (code[i].op != script::Op::ACTION || (code[i].actionId != kGetGlobalBoolean && code[i].actionId != kGetGlobalNumber)
                || code[i - 1].opcode != 0x04 || code[i - 1].qualifier != 0x05) {
                continue;
            }
            const std::string name = lowered(std::string(code[i - 1].bytes.begin(), code[i - 1].bytes.end()));
            std::int32_t wanted = 1;
            if (i + 2 < code.size() && code[i + 1].opcode == 0x04 && code[i + 1].qualifier == 0x03) {
                const std::int32_t constant = code[i + 1].operand0;
                switch (code[i + 2].op) {
                case script::Op::EQUAL: wanted = constant; break;
                case script::Op::NEQUAL: wanted = constant == 0 ? 1 : 0; break;
                case script::Op::GT: wanted = constant + 1; break;
                case script::Op::LT: wanted = constant - 1; break;
                default: wanted = constant; break;
                }
            } else if (i + 1 < code.size() && code[i + 1].op == script::Op::NOT) {
                wanted = 0;
            }
            out.emplace(name, wanted);
        }
        // The rest of the way there, from the walk's own path. A name it wants two ways stays unset.
        if (edge.gates.empty()) {
            return out;
        }
        std::map<std::string, std::int32_t> fromPath;
        std::set<std::string> torn;
        for (const Req& req : edge.gates.front()) {
            if (req.kind != "global" || req.opaque() || !req.text.empty()) {
                continue;
            }
            const std::int32_t value = static_cast<std::int32_t>(req.value);
            const std::int32_t wanted = req.op == "true" ? 1
                : req.op == "false"                      ? 0
                : req.op == "gt"                         ? value + 1
                : req.op == "lt"                         ? value - 1
                : req.op == "ne"                         ? (value == 0 ? 1 : 0)
                                                         : value;
            const std::string name = lowered(req.name);
            if (const auto [it, fresh] = fromPath.emplace(name, wanted); !fresh && it->second != wanted) {
                torn.insert(name);
            }
        }
        for (const auto& [name, wanted] : fromPath) {
            if (!torn.contains(name) && name != written) {
                out.emplace(name, wanted);
            }
        }
        return out;
    }

    // ---- edges one run could not settle ---------------------------------------------------

    static std::string familyOf(const std::string& reason) { return reason.substr(0, reason.find(':')); }

    static bool wantsNoFurtherSweep(const std::string& family)
    {
        return family == "opaque" || family == "effect_missing_mock" || family == "condition_false_opaque"
            || family == "condition_false_mock" || family == "unsatisfied" || family == "owner_not_available";
    }

    // Every value shipped code writes to a global, or nothing if a write is not a constant.
    std::optional<std::set<std::int64_t>> writerValues(const Req* req)
    {
        if (req->kind != "global") {
            return std::nullopt;
        }
        if (writers.empty()) {
            for (const Edge& edge : graph.edges) {
                if (edge.effect.kind == "global_set") {
                    Writers& entry = writers[lowered(edge.effect.name)];
                    if (edge.effect.valueKnown) {
                        entry.values.insert(edge.effect.value);
                    } else {
                        entry.dynamic = true;
                    }
                }
            }
        }
        const auto found = writers.find(lowered(req->name));
        if (found == writers.end() || found->second.dynamic) {
            return std::nullopt;
        }
        return found->second.values;
    }
    struct Writers {
        std::set<std::int64_t> values;
        bool dynamic = false;
    };
    std::map<std::string, Writers> writers;

    // An infeasible gate names one variable with two values because a gate keeps only the first
    // arm of each disjunction, so the clash alone proves nothing. This runs the real host for each
    // value the gates name (and its neighbours): a value that works settles the edge. None working
    // proves nothing, since other variables stay as solved: the edge stays can't-determine.
    void sweepInfeasible(const std::vector<const Edge*>& group, std::map<std::string, EdgeResult>& results)
    {
        constexpr std::size_t kMostAssignments = 64;
        std::map<std::string, std::set<std::int64_t>> domains;
        std::map<std::string, std::vector<const Req*>> constraints;
        for (const Edge* edge : group) {
            if (edge->gates.empty()) {
                continue;
            }
            std::map<std::string, std::vector<const Req*>> one;
            for (const Req& req : edge->gates.front()) {
                if (!req.opaque() && req.text.empty()) {
                    one[req.kind + "|" + req.owner + "|" + req.name].push_back(&req);
                }
            }
            for (const auto& [variable, reqs] : one) {
                if (!solve(reqs).has_value()) {
                    std::set<std::int64_t>& domain = domains[variable];
                    domain.insert({0, 1});
                    // A global holds what shipped code writes to it, or its default.
                    if (const auto values = writerValues(reqs.front()); values.has_value()) {
                        domain.insert(values->begin(), values->end());
                        continue;
                    }
                    for (const Req* req : reqs) {
                        domain.insert({req->value - 1, req->value, req->value + 1});
                    }
                }
            }
        }
        if (domains.empty()) {
            return;
        }
        std::vector<std::map<std::string, std::int64_t>> assignments{{}};
        bool complete = true;
        for (const auto& [variable, domain] : domains) {
            std::vector<std::map<std::string, std::int64_t>> next;
            for (const auto& partial : assignments) {
                for (const std::int64_t value : domain) {
                    if (next.size() >= kMostAssignments) {
                        complete = false;
                        break;
                    }
                    auto extended = partial;
                    extended[variable] = value;
                    next.push_back(std::move(extended));
                }
            }
            assignments = std::move(next);
        }
        std::vector<const Edge*> open = group;
        std::set<std::string> blocked;
        for (const auto& assignment : assignments) {
            if (open.empty()) {
                break;
            }
            Variant trial;
            trial.forced = assignment;
            variant = &trial;
            std::map<std::string, EdgeResult> scratch;
            runGroup(open, 0, scratch);
            variant = nullptr;
            std::vector<const Edge*> still;
            for (const Edge* edge : open) {
                const EdgeResult& seen = scratch[edge->id];
                if (seen.status == Status::Pass) {
                    results[edge->id] = {Status::Pass, seen.reason};
                    continue;
                }
                if (wantsNoFurtherSweep(familyOf(seen.reason))) {
                    blocked.insert(edge->id);
                }
                still.push_back(edge);
            }
            open = std::move(still);
        }
        // No value tried working is not a proof: the domain comes from the gate, the walk keeps one
        // arm of a disjunction, and only the clashing variable is varied. The directed search goes
        // on from here; it decides nothing dead either.
        (void)complete;
        (void)blocked;
    }

    // A query answered once may be asked several times, or ask a real handler's question better
    // left to it: try the real handler, then a "yes" at each call in turn.
    void sweepAnswered(const std::vector<const Edge*>& group, std::map<std::string, EdgeResult>& results)
    {
        constexpr int kMostCalls = 6;
        if (group.front()->gates.empty()) {
            return;
        }
        std::set<std::string> names;
        for (const Req& req : group.front()->gates.front()) {
            if (req.kind == "call" && req.name != "pc~self") {
                names.insert(req.name);
            }
        }
        std::vector<const Edge*> open = group;
        const auto attempt = [&](const Variant& trial) {
            variant = &trial;
            std::map<std::string, EdgeResult> scratch;
            runGroup(open, 0, scratch);
            variant = nullptr;
            std::vector<const Edge*> still;
            for (const Edge* edge : open) {
                const EdgeResult& seen = scratch[edge->id];
                if (seen.status == Status::Pass) {
                    results[edge->id] = seen;
                } else {
                    still.push_back(edge);
                }
            }
            open = std::move(still);
        };
        for (const std::string& name : names) {
            if (open.empty()) {
                return;
            }
            Variant real;
            real.realCalls.insert(name);
            attempt(real);
            for (int hit = 0; hit < kMostCalls && !open.empty(); ++hit) {
                Variant trial;
                trial.sequenceCall = name;
                trial.sequenceHit = hit;
                attempt(trial);
            }
        }
    }

    template <typename TimeUp>
    void sweep(const std::vector<const Edge*>& edges, std::map<std::string, EdgeResult>& results, TimeUp timeUp)
    {
        std::map<std::string, std::vector<const Edge*>> infeasible;
        std::map<std::string, std::vector<const Edge*>> answered;
        for (const Edge* edge : edges) {
            const auto found = results.find(edge->id);
            if (found == results.end() || found->second.status != Status::Cant) {
                continue;
            }
            const std::string& reason = found->second.reason;
            const std::string family = familyOf(reason);
            std::string key = edge->host.id() + "#";
            if (family == "infeasible" || (family == "condition_vs_branch" && reason.find("infeasible|") != std::string::npos)) {
                infeasible[key].push_back(edge);
            } else if (family == "effect_missing_mock" || family == "condition_false_mock") {
                for (const Req& req : edge->gates.empty() ? ReqSet{} : edge->gates.front()) {
                    key += req.key() + ";";
                }
                answered[key].push_back(edge);
            }
        }
        std::ofstream* log = failLog;
        failLog = nullptr; // A value the sweep forces need not be one a real run ever holds.
        // KOTOR_QUEST_DIAG: which sweep of which host grew the peak.
        std::size_t diagPeak = peakMemoryMb();
        const auto diagAfter = [&](const char* what, const std::string& key) {
            if (peakMemoryMb() > diagPeak + 100 && SDL_getenv("KOTOR_QUEST_DIAG") != nullptr) {
                std::printf("quest trace diag: %s %s peak %zu -> %zuMB\n", what, key.substr(0, 120).c_str(), diagPeak, peakMemoryMb());
                std::fflush(stdout);
            }
            diagPeak = peakMemoryMb();
        };
        for (const auto& [key, group] : infeasible) {
            if (timeUp()) {
                break;
            }
            sweepInfeasible(group, results);
            diagAfter("sweepInfeasible", key);
        }
        for (const auto& [key, group] : answered) {
            if (timeUp()) {
                break;
            }
            sweepAnswered(group, results);
            diagAfter("sweepAnswered", key);
        }
        std::map<std::string, std::vector<const Edge*>> directed;
        for (const Edge* edge : edges) {
            const auto found = results.find(edge->id);
            if (found == results.end() || found->second.status != Status::Cant) {
                continue;
            }
            const std::string family = familyOf(found->second.reason);
            if (family != "script_not_shipped" && family != "dlg_not_found" && family != "debug_script") {
                directed[edge->host.id()].push_back(edge);
            }
        }
        for (const auto& [key, group] : directed) {
            if (timeUp()) {
                break;
            }
            sweepDirected(group, results);
            diagAfter("sweepDirected", key);
        }
        failLog = log;
    }

    void clearAfterRun()
    {
        g.activeDialogue_.reset();
    }

    // ---- stage 3 ------------------------------------------------------------------------

    static std::string doorHow(const kotor::Entity& door, const Graph& graph)
    {
        if (!door.locked) {
            return "unlocked";
        }
        std::string how;
        if (door.keyRequired || !door.keyName.empty()) {
            how = "key:" + door.keyName;
        }
        if (!door.keyRequired) {
            how += how.empty() ? "" : ",";
            how += "security:" + std::to_string(door.openLockDC);
        }
        const auto openers = graph.doorOpeners.find(lowered(door.tag));
        if (openers != graph.doorOpeners.end() && !openers->second.empty()) {
            how += how.empty() ? "" : ",";
            how += "script:" + openers->second.front().script;
        }
        return how.empty() ? std::string("none") : how;
    }

    // Walks from `from` to the subject, opening doors in the query only. Returns the row
    // status; `doors` lists what stood in the way and how each opens.
    std::string reach(kotor::Vec3 from, std::size_t subject, std::vector<char> closed, std::string& doors, float& closest)
    {
        constexpr float kReach = 4.0f;
        nav::NavMesh* mesh = g.navMesh();
        const kotor::Entity& target = g.scene_.entities[subject];
        const float radius = g.creatureRadius(g.controlledObject());
        bool noOpener = false;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const nav::PathResult path = mesh->findPath(from, target.position, radius, closed);
            if (path.status == nav::PathStatus::Reached) {
                return noOpener ? "door_no_opener" : (doors.empty() ? "reachable" : "reachable_via_doors");
            }
            const kotor::Vec3 end = path.points.empty() ? from : path.points.back();
            const float dx = end.x - target.position.x;
            const float dy = end.y - target.position.y;
            const bool close = dx * dx + dy * dy <= kReach * kReach;
            closest = std::min(closest, std::sqrt(dx * dx + dy * dy));
            if (path.status == nav::PathStatus::Adjusted || path.status == nav::PathStatus::Unreachable) {
                if (close) {
                    return noOpener ? "door_no_opener" : (doors.empty() ? "reachable" : "reachable_via_doors");
                }
                return "unreachable";
            }
            if (path.status != nav::PathStatus::DoorBlocked || path.blockingDoor < 0) {
                return "unreachable";
            }
            const std::size_t line = static_cast<std::size_t>(path.blockingDoor);
            const std::size_t doorEntity = mesh->doors()[line].entity;
            if (doorEntity == subject) {
                return noOpener ? "door_no_opener" : (doors.empty() ? "reachable" : "reachable_via_doors");
            }
            const kotor::Entity& door = g.scene_.entities[doorEntity];
            const std::string how = doorHow(door, graph);
            if (how == "none") {
                noOpener = true;
            }
            doors += (doors.empty() ? "" : ";") + door.tag + "=" + how;
            for (std::size_t i = 0; i < mesh->doors().size(); ++i) {
                if (mesh->doors()[i].entity == doorEntity) {
                    closed[i] = 0;
                }
            }
        }
        return "unreachable";
    }

    // Walkmesh parts joined whatever the doors do, labelled from the arrival points. A
    // subject on a part no arrival touches has no walking route at all.
    std::vector<int> partOfEachTri(const nav::NavMesh& mesh, std::vector<std::size_t>& sizes) const
    {
        std::vector<int> part(mesh.tris().size(), -1);
        for (std::size_t start = 0; start < part.size(); ++start) {
            if (part[start] >= 0) {
                continue;
            }
            const int label = static_cast<int>(sizes.size());
            std::size_t count = 0;
            std::vector<std::size_t> open{start};
            part[start] = label;
            while (!open.empty()) {
                const std::size_t at = open.back();
                open.pop_back();
                ++count;
                const nav::Tri& tri = mesh.tris()[at];
                for (int i = 0; i < tri.linkCount; ++i) {
                    const int to = mesh.links()[static_cast<std::size_t>(tri.firstLink + i)].to;
                    if (to >= 0 && part[static_cast<std::size_t>(to)] < 0) {
                        part[static_cast<std::size_t>(to)] = label;
                        open.push_back(static_cast<std::size_t>(to));
                    }
                }
            }
            sizes.push_back(count);
        }
        return part;
    }

    // Distance from `point` to the nearest walkable corner of the parts in `reached`.
    static float gapTo(const nav::NavMesh& mesh, const std::vector<int>& part, const std::set<int>& reached, kotor::Vec3 point)
    {
        float best = 1.0e9f;
        for (std::size_t i = 0; i < part.size(); ++i) {
            if (reached.contains(part[i])) {
                for (const kotor::Vec3& corner : mesh.tris()[i].v) {
                    best = std::min(best, std::hypot(corner.x - point.x, corner.y - point.y));
                }
            }
        }
        return best;
    }

    // Which parts the module's doors touch, for the note beside an unreachable row.
    std::string doorParts(const nav::NavMesh& mesh, const std::vector<int>& part) const
    {
        std::string out;
        for (const nav::DoorLine& door : mesh.doors()) {
            if (door.entity >= g.scene_.entities.size()) {
                continue;
            }
            std::set<int> touched;
            for (std::size_t i = 0; i < part.size(); ++i) {
                for (const kotor::Vec3& corner : mesh.tris()[i].v) {
                    const float toA = std::hypot(corner.x - door.a.x, corner.y - door.a.y);
                    const float toB = std::hypot(corner.x - door.b.x, corner.y - door.b.y);
                    if (std::min(toA, toB) < 3.0f) {
                        touched.insert(part[i]);
                    }
                }
            }
            out += " door:" + g.scene_.entities[door.entity].tag + "=";
            for (const int label : touched) {
                out += std::to_string(label) + "+";
            }
        }
        return out;
    }

    static constexpr float kReachFromFloor = 4.0f;

    void navStage(const std::vector<const Edge*>& edges, std::vector<NavRow>& rows)
    {
        restore();
        nav::NavMesh* mesh = g.navMesh();
        std::set<std::string> done;
        const bool mainPath = graph.mainPath.contains(module != nullptr ? module->name : std::string{});
        std::vector<char> closed;
        if (mesh != nullptr) {
            closed.resize(mesh->doors().size(), 0);
            for (std::size_t i = 0; i < closed.size(); ++i) {
                const std::size_t entity = mesh->doors()[i].entity;
                closed[i] = entity < g.scene_.entities.size() && !g.scene_.entities[entity].open ? 1 : 0;
            }
        }
        // Arrival points: the module's entry, then every door or trigger leading elsewhere.
        std::vector<std::pair<std::string, kotor::Vec3>> arrivals{{"entry", g.scene_.entry}};
        for (const kotor::Entity& entity : g.scene_.entities) {
            if (!entity.linkedModule.empty()) {
                arrivals.emplace_back(entity.tag, entity.position);
            }
        }
        for (const Edge* edge : edges) {
            const std::string& tag = edge->host.ownerTag;
            if (tag.empty() || !done.insert(lowered(tag)).second) {
                continue;
            }
            NavRow row;
            row.module = edge->module;
            row.subject = tag;
            row.kind = edge->host.ownerKind;
            row.mainPath = mainPath;
            const std::size_t subject = g.findEntity(tag);
            const bool placed = module != nullptr
                && std::any_of(module->objects.begin(), module->objects.end(), [&](const quest::ObjectInfo& object) {
                       return object.placed && lowered(object.tag) == lowered(tag);
                   });
            if (subject == static_cast<std::size_t>(-1) || !placed) {
                row.status = "not_placed";
                rows.push_back(std::move(row));
                continue;
            }
            if (mesh == nullptr || mesh->empty()) {
                row.status = "no_mesh";
                rows.push_back(std::move(row));
                continue;
            }
            std::string best = "unreachable";
            float closest = 1.0e9f;
            for (const auto& [name, point] : arrivals) {
                std::string doors;
                const std::string status = reach(point, subject, closed, doors, closest);
                if (status == "reachable" || status == "reachable_via_doors") {
                    best = status;
                    row.from = name;
                    row.doors = doors;
                    break;
                }
                if (status == "door_no_opener" && best == "unreachable") {
                    best = status;
                    row.from = name;
                    row.doors = doors;
                }
            }
            row.status = best;
            if (best == "unreachable") {
                std::vector<std::size_t> sizes;
                const std::vector<int> part = partOfEachTri(*mesh, sizes);
                std::set<int> reached;
                for (const auto& [name, point] : arrivals) {
                    kotor::Vec3 snapped;
                    if (const int tri = mesh->nearest(point, snapped); tri >= 0) {
                        reached.insert(part[static_cast<std::size_t>(tri)]);
                    }
                }
                const kotor::Vec3 at = g.scene_.entities[subject].position;
                const float gap = gapTo(*mesh, part, reached, at);
                // Across a counter or a railing: within reach of the walkable floor.
                if (gap <= kReachFromFloor) {
                    row.status = "reachable_near";
                    row.doors = "gap=" + std::to_string(static_cast<int>(gap)) + "m";
                } else {
                    kotor::Vec3 snapped;
                    const int tri = mesh->nearest(at, snapped);
                    row.doors = "gap=" + std::to_string(static_cast<int>(gap)) + "m subject_part="
                        + (tri >= 0 ? std::to_string(part[static_cast<std::size_t>(tri)]) : std::string("none")) + " arrival_parts=";
                    for (const int label : reached) {
                        row.doors += std::to_string(label) + "+";
                    }
                    row.doors += " part_sizes=";
                    for (const std::size_t size : sizes) {
                        row.doors += std::to_string(size) + "+";
                    }
                    row.doors += doorParts(*mesh, part);
                }
            }
            rows.push_back(std::move(row));
        }
    }
};

} // namespace ke
