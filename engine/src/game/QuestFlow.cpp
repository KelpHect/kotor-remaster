// Sequence proofs on one real session: docs/recon/64_QUEST_TRACE.md section 9.
//
// --conversation-flow walks shipped dialogues node to node from an untouched module, choosing only
// replies whose real conditions passed. The only choice made for it is which reply to take.
//
// --plot-replay runs the main plot chain from the first module to each ending in one process, one
// setter edge at a time, on the state earlier steps left. No walking and no combat happen; what
// stands in for them, and anything else done outside the shipped path, is a listed bypass. A bypass
// of a kind that hides a plot-order problem fails the gate unless a baseline row names and explains
// it. Neither mode runs frames of play, so neither proves timing, AI or what a player can win.
#include "game/QuestTraceOps.h"

#include <filesystem>

namespace ke {

namespace {

struct FlowBaseline {
    std::map<std::string, std::string> exceptions; // "kind|subject" -> why.
};

FlowBaseline readFlowBaseline(const std::string& path)
{
    FlowBaseline out;
    std::ifstream file(path, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::vector<std::string> parts;
        std::stringstream stream(line);
        std::string part;
        while (std::getline(stream, part, '\t')) {
            parts.push_back(part);
        }
        if (parts.size() >= 3) {
            out.exceptions[parts[0] + "|" + parts[1]] = parts[2];
        }
    }
    return out;
}

std::vector<std::string> splitCommas(const std::string& text)
{
    std::vector<std::string> out;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, ',')) {
        if (!part.empty()) {
            out.push_back(part);
        }
    }
    return out;
}

std::optional<std::vector<script::Value>> droppedAction(ActionCall&)
{
    return std::vector<script::Value>{};
}

} // namespace

struct QuestFlowOps {
    GameSession& g;
    const Graph& graph;
    QuestTraceOps t;
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    QuestFlowOps(GameSession& game, const Graph& built) : g(game), graph(built), t{game, built} {}

    double seconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count(); }

    void breakGlobals()
    {
        t.originals[QuestTraceOps::kSetGlobalBoolean] = droppedAction;
        t.originals[QuestTraceOps::kSetGlobalNumber] = droppedAction;
    }

    const quest::DlgInfo* dialogueInfo(const std::string& module, const std::string& resref) const
    {
        if (const auto found = graph.dialogues.find(module + "|" + resref); found != graph.dialogues.end()) {
            return &found->second;
        }
        const auto found = graph.dialogues.find("|" + resref);
        return found != graph.dialogues.end() ? &found->second : nullptr;
    }

    // Replies from each entry that can reach a node not yet seen, by the static links.
    static bool leadsTo(const quest::DlgInfo& info, std::uint32_t reply, const std::set<std::pair<bool, std::uint32_t>>& seen)
    {
        std::set<std::pair<bool, std::uint32_t>> visited;
        std::vector<std::pair<bool, std::uint32_t>> open{{true, reply}};
        while (!open.empty()) {
            const auto at = open.back();
            open.pop_back();
            if (!visited.insert(at).second) {
                continue;
            }
            if (!seen.contains(at)) {
                return true;
            }
            const auto& nodes = at.first ? info.replies : info.entries;
            if (at.second < nodes.size()) {
                for (const auto& [target, active] : nodes[at.second].links) {
                    open.emplace_back(!at.first, target);
                }
            }
        }
        return false;
    }

    // ---- conversation flow ----------------------------------------------------------------

    struct FlowCounts {
        std::size_t dialogues = 0;
        std::size_t paths = 0;
        std::size_t nodesSeen = 0;
        std::size_t nodesTotal = 0;
        std::size_t blockedAtStart = 0;
        std::size_t noOwner = 0;
        std::size_t effectsChecked = 0;
        std::size_t loopsCut = 0;
        std::size_t pausedOut = 0;
        std::size_t moduleLeft = 0;
        std::size_t nodesSeeded = 0; // Seen only on a state the plot replay left.
        std::size_t nodesByCondition = 0; // Seen only with a link's own condition written true.
        std::size_t seedRuns = 0;
    };

    // What the plot replay had written when it reached a module. Used as the state a conversation
    // starts on: everything in it was written by shipped scripts on the replay's route.
    struct PlotState {
        std::string module;
        decltype(script::World::globals) globals;
        std::map<std::string, std::int32_t> journal;
    };
    const PlotState* flowSeed = nullptr;
    const ReqSet* flowReqs = nullptr;
    std::map<std::string, std::set<std::pair<bool, std::uint32_t>>> flowSeen;

    // Pumps until a script's pause is over. False if it never ends without frames of play.
    bool waitOutPause()
    {
        t.pump([&]() { return !g.sim_->dialoguePaused; });
        return !g.sim_->dialoguePaused;
    }

    void walkDialogue(const std::string& module, const std::string& resref, std::size_t ownerIndex,
                      const std::vector<const Edge*>& edges, FlowCounts& counts,
                      std::vector<std::pair<std::string, std::string>>& failures, std::ofstream& out)
    {
        constexpr int kMostPaths = 12;
        constexpr int kMostSteps = 128;
        const quest::DlgInfo* info = dialogueInfo(module, resref);
        std::set<std::pair<bool, std::uint32_t>>& seen = flowSeen[resref];
        const std::size_t seenAtStart = seen.size();
        std::set<std::string> reported;
        const auto fail = [&](const std::string& kind, const std::string& detail) {
            const std::string subject = module + ":" + resref + (detail.empty() ? "" : ":" + detail);
            if (reported.insert(kind + "|" + subject).second) {
                failures.emplace_back(kind, subject);
            }
        };
        if (flowSeed == nullptr && flowReqs == nullptr) {
            ++counts.dialogues;
        }
        int paths = 0;
        std::size_t endsSeen = 0;
        for (; paths < kMostPaths; ++paths) {
            t.restore();
            if (flowSeed != nullptr) {
                g.scriptWorld_.globals = flowSeed->globals;
                g.journal_ = flowSeed->journal;
            }
            if (flowReqs != nullptr) {
                QuestTraceOps::Established established;
                t.establish(*flowReqs, g.objectForEntity(ownerIndex), established);
            }
            g.dialogueTrail_.clear();
            g.traceDialogueTrail_ = true;
            const std::size_t before = seen.size();
            bool started = false;
            try {
                started = g.startDialogue(ownerIndex);
            } catch (const std::exception& exception) {
                fail("exception", exception.what());
            }
            if (!started) {
                if (paths == 0 && flowSeed == nullptr && flowReqs == nullptr) {
                    ++counts.blockedAtStart;
                }
                break;
            }
            ++counts.paths;
            counts.seedRuns += flowSeed != nullptr || flowReqs != nullptr ? 1 : 0;
            int steps = 0;
            bool leftModule = false;
            while (g.dialogueView_.active && g.activeDialogue_.has_value()) {
                if (++steps > kMostSteps) {
                    // Every reply left only leads round again.
                    ++counts.loopsCut;
                    g.finishDialogue(true);
                    break;
                }
                if (!g.pendingModule_.empty()) {
                    leftModule = true;
                }
                if (g.sim_->dialoguePaused && !waitOutPause()) {
                    ++counts.pausedOut;
                    g.finishDialogue(true);
                    break;
                }
                const std::vector<DialogueOption>& options = g.dialogueView_.options;
                if (options.empty()) {
                    fail("no_options", std::to_string(g.dialogueView_.entry));
                    g.finishDialogue(true);
                    break;
                }
                std::size_t choice = 0;
                if (info != nullptr) {
                    bool found = false;
                    for (std::size_t i = 0; i < options.size() && !found; ++i) {
                        if (options[i].reply != UINT32_MAX && !seen.contains({true, options[i].reply})) {
                            choice = i;
                            found = true;
                        }
                    }
                    for (std::size_t i = 0; i < options.size() && !found; ++i) {
                        if (options[i].reply != UINT32_MAX && leadsTo(*info, options[i].reply, seen)) {
                            choice = i;
                            found = true;
                        }
                    }
                }
                try {
                    g.chooseDialogueOption(choice);
                } catch (const std::exception& exception) {
                    fail("exception", exception.what());
                    g.clearDialogueState();
                    break;
                }
                for (const auto& node : g.dialogueTrail_) {
                    seen.insert(node);
                }
            }
            leftModule = leftModule || !g.pendingModule_.empty();
            counts.moduleLeft += leftModule ? 1 : 0;
            t.pump([]() { return false; });
            for (const auto& node : g.dialogueTrail_) {
                seen.insert(node);
            }
            g.traceDialogueTrail_ = false;
            // Each script run in this conversation, and the unconditional effect of each node.
            if (const std::string problem = t.vmProblem(); !problem.empty()) {
                fail("vm", problem.substr(0, 96));
            }
            for (const std::string& line : g.scriptLog_) {
                if (line.find("refused:") != std::string::npos) {
                    fail("script_refused", line.substr(0, 96));
                }
            }
            for (const auto& [isReply, node] : g.dialogueTrail_) {
                for (const Edge* edge : edges) {
                    if (edge->host.dlg != resref || edge->host.node != node
                        || edge->host.kind != (isReply ? "dlg_reply" : "dlg_entry")) {
                        continue;
                    }
                    const bool unconditional = !edge->deferred
                        && (edge->gates.empty() || std::all_of(edge->gates.begin(), edge->gates.end(),
                                                               [](const ReqSet& reqs) { return reqs.empty(); }));
                    if (!unconditional || t.scriptMissing(edge->siteScript)) {
                        continue;
                    }
                    const Effect& effect = edge->effect;
                    bool held = true;
                    if (effect.kind == "journal_set") {
                        // A later node of this path may take the entry away again: k_pebo_sasha01 does.
                        const bool removedOnPath = std::any_of(edges.begin(), edges.end(), [&](const Edge* other) {
                            return other->effect.kind == "journal_remove" && other->host.dlg == resref
                                && lowered(other->effect.name) == lowered(effect.name)
                                && std::find(g.dialogueTrail_.begin(), g.dialogueTrail_.end(),
                                             std::pair<bool, std::uint32_t>{other->host.kind == "dlg_reply", other->host.node})
                                    != g.dialogueTrail_.end();
                        });
                        if (removedOnPath) {
                            continue;
                        }
                        held = g.journalState(effect.name) >= std::abs(effect.value);
                    } else if (effect.kind == "global_set" && effect.valueKnown && effect.aux.rfind("str", 0) != 0) {
                        if (t.droppedGlobals.contains(lowered(effect.name))) {
                            continue; // Retail drops it too (00529030).
                        }
                        // The write happened, and the store holds the last one made.
                        const std::string name = lowered(effect.name);
                        std::optional<std::int64_t> last;
                        bool wrote = false;
                        for (const auto& [written, value] : t.globalWrites) {
                            if (written == name) {
                                last = value;
                                wrote = wrote || value == effect.value;
                            }
                        }
                        held = wrote && last == g.scriptWorld_.getGlobal(effect.name, script::ValueType::Int).integer;
                    } else {
                        continue;
                    }
                    ++counts.effectsChecked;
                    if (!held) {
                        fail("effect_missing", edge->id);
                    }
                }
            }
            for (const auto& node : g.dialogueTrail_) {
                out << module << '\t' << resref << '\t' << paths << '\t' << (node.first ? 'R' : 'E') << node.second << '\n';
            }
            endsSeen += g.activeDialogue_.has_value() ? 0 : 1;
            g.pendingModule_.clear();
            g.sim_->pendingLinkedTo.clear();
            // Done once a path found nothing new.
            if (seen.size() == before) {
                break;
            }
        }
        if (flowReqs != nullptr) {
            counts.nodesByCondition += seen.size() - seenAtStart;
            return;
        }
        if (flowSeed != nullptr) {
            counts.nodesSeeded += seen.size() - seenAtStart;
            return;
        }
        counts.nodesSeen += seen.size();
        if (info != nullptr) {
            counts.nodesTotal += info->entries.size() + info->replies.size();
        }
        (void)endsSeen;
    }

    // ---- plot replay ----------------------------------------------------------------------

    struct Bypass {
        std::string kind;
        std::string subject;
        std::string reason;
    };
    std::vector<Bypass> bypasses;
    std::set<std::string> usedBypassKeys;
    std::map<std::string, std::vector<const Edge*>> setters; // "global|name", "journal|name"
    std::set<std::string> edgesDone;
    std::set<std::string> edgesRunning;
    bool tripped = false; // A trip for a precondition reloaded the module: owners are stale.
    int talksAgain = 0;
    std::set<std::pair<std::uint32_t, std::uint32_t>> avoidReplies;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> lastReply;
    std::map<std::string, const Edge*> edgeById;
    std::string dropSetter;                                  // Negative check: this global is never written.
    static inline const std::string* droppedName = nullptr;
    static inline ActionHandler droppedNumberNext = nullptr;
    std::ofstream* replayOut = nullptr;
    std::size_t legIndex = 0;
    std::size_t edgesRun = 0;
    std::size_t edgesFailed = 0;
    std::size_t saves = 0;
    std::size_t saveMismatches = 0;

    void note(const std::string& kind, const std::string& subject, const std::string& reason)
    {
        if (usedBypassKeys.insert(kind + "|" + subject).second) {
            bypasses.push_back({kind, subject, reason});
        }
    }

    void indexSetters()
    {
        for (const Edge& edge : graph.edges) {
            edgeById[edge.id] = &edge;
            if (edge.debug || edge.orphan || !edge.dead.empty()) {
                continue;
            }
            if (edge.effect.kind == "global_set" && edge.effect.nameKnown) {
                setters["global|" + lowered(edge.effect.name)].push_back(&edge);
            } else if (edge.effect.kind == "journal_set" && edge.effect.nameKnown) {
                setters["journal|" + lowered(edge.effect.name)].push_back(&edge);
            }
        }
    }

    std::int64_t globalNow(const std::string& name) const
    {
        const bool known = g.runtime_->globalDeclared(name, script::GlobalKind::Number)
            || g.runtime_->globalDeclared(name, script::GlobalKind::Boolean);
        return known ? g.scriptWorld_.getGlobal(name, script::ValueType::Int).integer : 0;
    }

    // Whether a precondition holds now. Empty: this harness cannot read it (a query).
    std::optional<bool> holds(const Req& req, std::uint32_t self)
    {
        if (req.kind == "global") {
            if (!req.text.empty()) {
                const bool same = lowered(g.scriptWorld_.getGlobal(req.name, script::ValueType::String).text.str()) == lowered(req.text);
                return req.op == "eq" ? same : !same;
            }
            return satisfies(globalNow(req.name), req.op, req.value);
        }
        if (req.kind == "journal") {
            return satisfies(g.journalState(req.name), req.op, req.value);
        }
        if (req.kind == "exists") {
            const std::uint32_t object = g.scriptWorld_.byTag(req.name, 0);
            const bool present = object != script::kObjectInvalid && g.scriptWorld_.valid(object);
            return satisfies(present ? 1 : 0, req.op, req.value);
        }
        if (req.kind == "item") {
            const std::uint32_t holder = t.resolveOwner(req.owner, self);
            const bool has = holder != script::kObjectInvalid && g.itemPossessedBy(holder, req.name) != script::kObjectInvalid;
            return satisfies(has ? 1 : 0, req.op, req.value);
        }
        if (req.kind == "local") {
            const std::uint32_t object = t.resolveOwner(req.owner, self);
            if (object == script::kObjectInvalid) {
                return false;
            }
            std::int64_t index = 0;
            try {
                index = std::stoll(req.name);
            } catch (const std::exception&) {
                return std::nullopt;
            }
            std::vector<std::vector<script::Value>> args{{script::Value::makeObject(object)}, {script::Value::makeInt(index)}};
            const auto number = g.actionAsForTest(self, 681, args);
            const auto boolean = index >= 0 && index < 96 ? g.actionAsForTest(self, 679, args) : std::nullopt;
            const std::int64_t value = number.has_value() && !number->empty() ? number->front().asInt() : 0;
            const std::int64_t flag = boolean.has_value() && !boolean->empty() ? boolean->front().asInt() : 0;
            return satisfies(value, req.op, req.value) || satisfies(flag, req.op, req.value);
        }
        return std::nullopt;
    }

    static std::string keyOfReq(const Req& req) { return req.kind + "|" + lowered(req.name); }

    // Setter edges that would make `req` true, this module first.
    std::vector<const Edge*> settersFor(const Req& req)
    {
        std::vector<const Edge*> out;
        const auto found = setters.find(keyOfReq(req));
        if (found == setters.end()) {
            return out;
        }
        for (const Edge* edge : found->second) {
            if (edge->effect.valueKnown
                && satisfies(req.kind == "journal" ? std::abs(edge->effect.value) : edge->effect.value, req.op, req.value)) {
                out.push_back(edge);
            }
        }
        std::stable_sort(out.begin(), out.end(), [&](const Edge* a, const Edge* b) {
            const bool hereA = a->module == g.module_ || a->module == "global";
            const bool hereB = b->module == g.module_ || b->module == "global";
            if (hereA != hereB) {
                return hereA;
            }
            const auto cost = [](const Edge* edge) { return edge->gates.empty() ? std::size_t{0} : edge->gates.front().size(); };
            return cost(a) < cost(b);
        });
        return out;
    }

    // Makes one precondition true with shipped edges, or says what stood in.
    bool achieve(const Req& req, std::uint32_t self, const Edge& forEdge, int depth)
    {
        const std::optional<bool> now = holds(req, self);
        if (now.value_or(true)) {
            return true;
        }
        if (req.kind == "exists") {
            const bool wanted = satisfies(1, req.op, req.value);
            if (!wanted) {
                const std::size_t index = g.findEntity(req.name);
                if (index != static_cast<std::size_t>(-1)) {
                    // Combat is not played: the real death path runs in its place.
                    note("kill", g.module_ + ":" + req.name, "combat is not played; the real death path ran");
                    g.onCreatureKilled(index, g.scriptWorld_.pc);
                    g.flushScriptEvents();
                    t.pump([]() { return false; });
                    return true;
                }
                return false;
            }
            if (t.spawnForTag(req.name) != script::kObjectInvalid) {
                note("spawned", g.module_ + ":" + req.name, "a template of this module spawned, not the shipped route to it");
                return true;
            }
            return false;
        }
        if (req.kind == "global" || req.kind == "journal") {
            // On a shut door's account, a plot number climbs through the values its own edges write, in
            // order, as a player would meet them (DAN_JEDI_PLOT 1, 2, 3, 4, then 7). A step that does not
            // take is simply passed: the condition still decides at the end.
            if (returning && depth < 6 && req.kind == "global" && req.op == "eq" && dropSetter != lowered(req.name)) {
                if (const auto all = setters.find(keyOfReq(req)); all != setters.end()) {
                    std::map<std::int64_t, std::vector<const Edge*>> byValue;
                    for (const Edge* edge : all->second) {
                        if (edge->effect.valueKnown && edge->effect.value < req.value && !edge->orphan && !edge->debug && edge->dead.empty()) {
                            byValue[edge->effect.value].push_back(edge);
                        }
                    }
                    for (const auto& [value, edges] : byValue) {
                        for (const Edge* edge : edges) {
                            if (globalNow(req.name) >= value || edgesRunning.contains(edge->id)) {
                                break;
                            }
                            const std::string from = lowered(g.module_);
                            std::vector<std::string> routeFailures;
                            if (edge->module != from && edge->module != "global") {
                                goTo(edge->module, "trip", routeFailures, false);
                            }
                            if (lowered(g.module_) == edge->module || edge->module == "global") {
                                runEdge(*edge, depth + 1, false);
                            }
                            if (lowered(g.module_) != from) {
                                goTo(from, "trip", routeFailures, false);
                                tripped = true;
                            }
                            if (lowered(g.module_) != from) {
                                return false;
                            }
                            self = g.scriptWorld_.pc;
                        }
                    }
                }
            }
            if (depth < 6) {
                for (const Edge* edge : settersFor(req)) {
                    if (dropSetter == lowered(req.name)) {
                        break;
                    }
                    if ((edge->module != g.module_ && edge->module != "global") || edgesRunning.contains(edge->id)) {
                        continue;
                    }
                    if (runEdge(*edge, depth + 1, false) && holds(req, self).value_or(true)) {
                        return true;
                    }
                }
                // On a shut door's account, its setter in another module, and back (the Jedi trials
                // run between danm13 and danm14ac). Never forced: no shipped route, no trip.
                if (returning && !holds(req, self).value_or(true)) {
                    const std::string from = lowered(g.module_);
                    std::vector<std::string> routeFailures;
                    for (const Edge* edge : settersFor(req)) {
                        if (edge->module == from || edge->module == "global" || edgesRunning.contains(edge->id) || edge->orphan || edge->debug || !edge->dead.empty()) {
                            continue;
                        }
                        if (SDL_getenv("KOTOR_QUEST_DIAG") != nullptr) {
                            std::printf("plot replay diag: trip from %s for %s to %s\n", from.c_str(), req.key().c_str(), edge->id.c_str());
                        }
                        goTo(edge->module, "trip", routeFailures, false);
                        if (lowered(g.module_) == edge->module) {
                            runEdge(*edge, depth + 1, false);
                        }
                        goTo(from, "trip", routeFailures, false);
                        tripped = true;
                        if (lowered(g.module_) != from) {
                            return false;
                        }
                        self = g.scriptWorld_.pc;
                        if (holds(req, self).value_or(false)) {
                            return true;
                        }
                    }
                }
            }
            return holds(req, self).value_or(true);
        }
        if (req.kind == "item" && satisfies(1, req.op, req.value)) {
            // A shipped edge of this module first: the item's own route.
            if (depth < 6) {
                for (const Edge& edge : graph.edges) {
                    if (edge.module == lowered(g.module_) && edge.effect.kind == "item_create" && lowered(edge.effect.name) == lowered(req.name)
                        && edge.dead.empty() && !edge.orphan && !edge.debug && !edgesRunning.contains(edge.id)
                        && runEdge(edge, depth + 1, false) && holds(req, self).value_or(true)) {
                        return true;
                    }
                }
            }
            const std::uint32_t holder = t.resolveOwner(req.owner, self);
            if (holder != script::kObjectInvalid) {
                note("item_given", g.module_ + ":" + req.name, "picked up in play this replay does not walk; CreateItemOnObject ran");
                std::vector<std::vector<script::Value>> args{{script::Value::makeString(req.name)}, {script::Value::makeObject(holder)}, {script::Value::makeInt(1)}};
                g.actionAsForTest(g.scriptWorld_.pc, 31, args);
                return holds(req, self).value_or(true);
            }
        }
        (void)forEdge;
        return false;
    }

    // The state a condition script on the way to a node reads: its easiest way of being true, reached
    // with shipped edges where the graph knows one.
    void achievePath(const Edge& edge, std::uint32_t self, int depth)
    {
        const auto module = std::find_if(graph.modules.begin(), graph.modules.end(),
                                         [&](const quest::ModuleInfo& info) { return info.name == edge.module; });
        for (const std::string& active : edge.pathActive) {
            const quest::ScriptFacts* facts = nullptr;
            const std::string name = lowered(active);
            if (module != graph.modules.end()) {
                if (const auto found = module->scripts.find(name); found != module->scripts.end()) {
                    facts = &graph.facts[found->second];
                }
            }
            if (facts == nullptr) {
                if (const auto found = graph.globalScripts.find(name); found != graph.globalScripts.end()) {
                    facts = &graph.facts[found->second];
                }
            }
            if (facts == nullptr || facts->truePaths.empty()) {
                continue;
            }
            const ReqSet* best = nullptr;
            std::size_t bestUnmet = SIZE_MAX;
            for (const ReqSet& reqs : facts->truePaths) {
                if (const std::size_t unmet = unmetCount(reqs, self); unmet < bestUnmet) {
                    bestUnmet = unmet;
                    best = &reqs;
                }
            }
            if (best != nullptr) {
                for (const Req& req : *best) {
                    if (!req.opaque()) {
                        achieve(req, self, edge, depth);
                    }
                }
            }
        }
    }

    void pumpAndDrain()
    {
        t.pump([]() { return false; });
        g.flushScriptEvents();
        if (g.partySelection().active) {
            note("party_panel", g.module_, "the party panel is accepted as it stands; its exit script ran");
            g.acceptPartySelection();
            t.pump([]() { return false; });
            g.flushScriptEvents();
        }
    }

    // Walks the real conversation to a node, taking at each step a reply that can still reach it.
    bool walkConversationTo(const Edge& edge, std::uint32_t owner, std::string& why)
    {
        const quest::Host& host = edge.host;
        const quest::DlgInfo* info = dialogueInfo(edge.module, host.dlg);
        if (info == nullptr) {
            why = "no_dialogue_data";
            return false;
        }
        const std::size_t ownerIndex = ScriptApi{g}.entityFor(owner);
        if (talksAgain == 0) {
            avoidReplies.clear();
            lastReply.reset();
        }
        g.dialogueTrail_.clear();
        g.traceDialogueTrail_ = true;
        struct Stop {
            GameSession& g;
            ~Stop() { g.traceDialogueTrail_ = false; }
        } stop{g};
        if (!g.activeDialogue_.has_value() || lowered(g.activeDialogue_->resref) != host.dlg) {
            g.clearDialogueState();
            std::string& conversation = ownerIndex != static_cast<std::size_t>(-1)
                ? g.scene_.entities[ownerIndex].conversation : why;
            const bool sameDlg = ownerIndex != static_cast<std::size_t>(-1) && lowered(conversation) == host.dlg;
            const bool started = g.startConversation(owner, sameDlg ? std::string{} : host.dlg);
            why.clear();
            if (!started) {
                why = g.sim_->noStartLink ? "no_start_link" : "conversation_did_not_start";
                std::set<std::string> wants;
                const std::pair<bool, std::uint32_t> goal{host.kind == "dlg_reply", host.node};
                for (const auto& [start, active] : info->starting) {
                    if (distanceTo(*info, {false, start}, goal, host.kind == "dlg_end" || host.kind == "dlg_abort") != SIZE_MAX) {
                        wants.insert(active.empty() ? std::string("(unconditional)") : active);
                    }
                }
                why += ":wants";
                for (const std::string& want : wants) {
                    why += " " + want;
                }
                return false;
            }
        }
        const std::pair<bool, std::uint32_t> target{host.kind == "dlg_reply", host.node};
        const bool anyEnd = host.kind == "dlg_end" || host.kind == "dlg_abort";
        const auto reached = [&]() {
            return !anyEnd && std::find(g.dialogueTrail_.begin(), g.dialogueTrail_.end(), target) != g.dialogueTrail_.end();
        };
        std::set<std::pair<std::uint32_t, std::uint32_t>> taken;
        for (int step = 0; step < 128; ++step) {
            if (reached()) {
                return true;
            }
            if (!g.dialogueView_.active || !g.activeDialogue_.has_value()) {
                if (anyEnd) {
                    return true;
                }
                why = "conversation_ended_before_node";
                return false;
            }
            if (g.sim_->dialoguePaused && !waitOutPause()) {
                why = "paused_no_resume";
                return false;
            }
            const std::vector<DialogueOption>& options = g.dialogueView_.options;
            std::optional<std::size_t> pick;
            std::size_t bestDistance = SIZE_MAX;
            for (std::size_t i = 0; i < options.size(); ++i) {
                if (options[i].reply == UINT32_MAX || taken.contains({g.dialogueView_.entry, options[i].reply})
                    || avoidReplies.contains({g.dialogueView_.entry, options[i].reply})) {
                    continue;
                }
                if (const std::size_t distance = distanceTo(*info, {true, options[i].reply}, target, anyEnd); distance < bestDistance) {
                    bestDistance = distance;
                    pick = i;
                }
            }
            if (!pick.has_value()) {
                why = anyEnd ? "" : "no_reply_reaches_node";
                if (!anyEnd && g.dialogueView_.entry < info->entries.size()) {
                    // The links from here that would reach the node, none of them offered.
                    why += ":at E" + std::to_string(g.dialogueView_.entry) + " wants";
                    bool any = false;
                    for (const auto& [to, active] : info->entries[g.dialogueView_.entry].links) {
                        if (distanceTo(*info, {true, to}, target, false) != SIZE_MAX) {
                            why += " " + (active.empty() ? std::string("(unconditional)") : active);
                            any = true;
                        }
                    }
                    if (!any && returning && talksAgain < 12) {
                        // Another start was taken, and many are one-shot (k_pdan_anger20, k_pdan_zhar05): on a
                        // shut door's account, talk this one through as a player would and start again. The reply
                        // that led here stays untaken: its link's persuasion check did not offer the node.
                        ++talksAgain;
                        if (lastReply.has_value()) {
                            avoidReplies.insert(*lastReply);
                        }
                        const std::uint32_t startedAt = g.dialogueView_.entry;
                        for (int line = 0; line < 64 && g.dialogueView_.active && g.activeDialogue_.has_value(); ++line) {
                            if (g.sim_->dialoguePaused && !waitOutPause()) {
                                break;
                            }
                            g.chooseDialogueOption(0);
                        }
                        pumpAndDrain();
                        g.clearDialogueState();
                        // A start that holds until something else clears it (k_pdan_saber15: talk to Dorak).
                        for (const auto& [start, active] : info->starting) {
                            if (start != startedAt || active.empty()) {
                                continue;
                            }
                            const auto found = graph.globalScripts.find(lowered(active));
                            const auto module = std::find_if(graph.modules.begin(), graph.modules.end(), [&](const quest::ModuleInfo& one) { return one.name == edge.module; });
                            const quest::ScriptFacts* facts = found != graph.globalScripts.end() ? &graph.facts[found->second] : nullptr;
                            if (module != graph.modules.end()) {
                                if (const auto local = module->scripts.find(lowered(active)); local != module->scripts.end()) {
                                    facts = &graph.facts[local->second];
                                }
                            }
                            if (facts != nullptr && facts->truePaths.size() == 1 && facts->truePaths.front().size() == 1) {
                                Req clear = *facts->truePaths.front().begin();
                                if (clear.kind == "global" && clear.op == "true") {
                                    clear.op = "false";
                                    for (const Edge* setter : settersFor(clear)) {
                                        if (setter->module == lowered(g.module_) && !edgesRunning.contains(setter->id)) {
                                            edgesDone.erase(setter->id);
                                            runEdge(*setter, 5, false);
                                        }
                                    }
                                }
                            }
                        }
                        why.clear();
                        const bool walked = walkConversationTo(edge, owner, why);
                        --talksAgain;
                        return walked;
                    }
                    if (!any) {
                        // Another start was taken: what the node's own start asks for.
                        why += " other start:";
                        for (const auto& [start, active] : info->starting) {
                            if (distanceTo(*info, {false, start}, target, false) != SIZE_MAX) {
                                why += " E" + std::to_string(start) + "=" + (active.empty() ? std::string("(unconditional)") : active);
                            }
                        }
                    }
                }
                g.chooseDialogueOption(0);
                if (anyEnd) {
                    continue;
                }
                return false;
            }
            taken.insert({g.dialogueView_.entry, options[*pick].reply});
            lastReply = std::pair<std::uint32_t, std::uint32_t>{g.dialogueView_.entry, options[*pick].reply};
            g.chooseDialogueOption(*pick);
        }
        why = "step_limit";
        return false;
    }

    static std::size_t distanceTo(const quest::DlgInfo& info, std::pair<bool, std::uint32_t> from, std::pair<bool, std::uint32_t> target, bool anyEnd)
    {
        std::set<std::pair<bool, std::uint32_t>> visited;
        std::vector<std::pair<bool, std::uint32_t>> level{from};
        for (std::size_t distance = 0; !level.empty(); ++distance) {
            std::vector<std::pair<bool, std::uint32_t>> next;
            for (const auto& at : level) {
                if (!visited.insert(at).second) {
                    continue;
                }
                const auto& nodes = at.first ? info.replies : info.entries;
                if (at.second >= nodes.size()) {
                    continue;
                }
                if ((!anyEnd && at == target) || (anyEnd && nodes[at.second].links.empty())) {
                    return distance;
                }
                for (const auto& [to, active] : nodes[at.second].links) {
                    next.emplace_back(!at.first, to);
                }
            }
            level = std::move(next);
        }
        return SIZE_MAX;
    }

    // Plain writes the negative check loses; everything else goes to the real handler.
    static std::optional<std::vector<script::Value>> droppingNumberWrite(ActionCall& call)
    {
        if (droppedName != nullptr && lowered(call.stringArg(0)) == *droppedName) {
            return std::vector<script::Value>{};
        }
        return droppedNumberNext != nullptr ? droppedNumberNext(call) : std::nullopt;
    }

    void dropGlobalWrites(const std::string& name)
    {
        dropSetter = name;
        droppedName = &dropSetter;
        droppedNumberNext = t.originals[QuestTraceOps::kSetGlobalNumber];
        t.originals[QuestTraceOps::kSetGlobalNumber] = droppingNumberWrite;
    }

    std::size_t unmetCount(const ReqSet& reqs, std::uint32_t self)
    {
        std::size_t unmet = 0;
        for (const Req& req : reqs) {
            if (!req.opaque() && !holds(req, self).value_or(true)) {
                ++unmet;
            }
        }
        return unmet;
    }

    // The alternative of an edge's gates closest to holding on the state as it stands.
    const ReqSet& pickGate(const Edge& edge, std::uint32_t self)
    {
        static const ReqSet none;
        const ReqSet* best = &none;
        std::size_t bestUnmet = SIZE_MAX;
        for (const ReqSet& reqs : edge.gates) {
            if (const std::size_t unmet = unmetCount(reqs, self); unmet < bestUnmet) {
                bestUnmet = unmet;
                best = &reqs;
            }
        }
        return *best;
    }

    // True when an earlier step, or the module's own entry scripts, already did this edge's work.
    bool alreadyHeld(const Edge& edge)
    {
        if (edge.effect.kind == "global_set" && edge.effect.valueKnown && edge.effect.aux.rfind("str", 0) != 0) {
            return globalNow(edge.effect.name) == edge.effect.value && edge.effect.value != 0;
        }
        if (edge.effect.kind == "journal_set" && edge.effect.valueKnown) {
            return g.journalState(edge.effect.name) >= std::abs(edge.effect.value);
        }
        return false;
    }

    // Runs one shipped edge on the state as it stands. Its gates are met by earlier edges where
    // the graph knows of one; each query it asks goes to its real handler first.
    bool runEdge(const Edge& edge, int depth, bool forceOnFailure)
    {
        if (dialoguesOf != lowered(g.module_)) {
            t.dialogues.clear();
            dialoguesOf = lowered(g.module_);
        }
        if (edgesDone.contains(edge.id) || alreadyHeld(edge) || !edgesRunning.insert(edge.id).second) {
            return true;
        }
        struct Done {
            std::set<std::string>& running;
            const std::string& id;
            ~Done() { running.erase(id); }
        } done{edgesRunning, edge.id};
        ++edgesRun;
        bool fallback = false;
        const std::uint32_t owner = t.ownerFor(edge.host, fallback);
        if (fallback) {
            // The owner of this edge is not in the module as this run reached it.
            const std::string resref = edge.host.ownerResref;
            if (!resref.empty()) {
                g.spawnScriptCreature(resref, g.position_, 0.0f);
            }
        }
        const bool wasMissing = fallback;
        std::uint32_t self = fallback ? t.ownerFor(edge.host, fallback) : owner;
        if (wasMissing) {
            // Nothing here says the shipped route makes this owner exist at this point.
            if (fallback) {
                note("pc_for_owner", g.module_ + ":" + edge.host.ownerTag, "the owner is neither placed nor spawnable; the PC ran it");
            } else {
                note("spawned", g.module_ + ":" + edge.host.ownerResref, "the owner is not placed here; its template spawned");
            }
        }
        const ReqSet& reqs = pickGate(edge, self);
        bool metAll = true;
        for (const Req& req : reqs) {
            if (!req.opaque() && !achieve(req, self, edge, depth)) {
                metAll = false;
            }
        }
        achievePath(edge, self, depth);
        if (returning && SDL_getenv("KOTOR_QUEST_DIAG") != nullptr) {
            std::printf("plot replay diag: on trip %s depth %d metAll %d module %s\n", edge.id.c_str(), depth, metAll ? 1 : 0, g.module_.c_str());
        }
        if (tripped) {
            tripped = false;
            bool again = false;
            self = t.ownerFor(edge.host, again);
        }
        QuestTraceOps::Before before;
        before.xp = g.character_.xp;
        if (edge.effect.kind == "global_set" && edge.effect.aux.rfind("str", 0) != 0) {
            before.globals[edge.effect.name] = g.scriptWorld_.getGlobal(edge.effect.name, script::ValueType::Int).integer;
        }
        t.globalWrites.clear();
        std::string why;
        const auto attempt = [&]() {
            why.clear();
            bool ran = true;
            try {
                if (!edge.host.dlg.empty()) {
                    ran = walkConversationTo(edge, self, why);
                } else {
                    const QuestTraceOps::RunOutcome outcome = t.runHost(edge.host, self, {});
                    if (outcome.threw || outcome.hostMissing) {
                        ran = false;
                        why = outcome.threw ? outcome.error.substr(0, 80) : "host_missing";
                    }
                }
            } catch (const std::exception& exception) {
                ran = false;
                why = exception.what();
            }
            pumpAndDrain();
            return ran && t.effectHeld(edge.effect, self, before);
        };
        bool ok = attempt();
        std::string detail = metAll ? why : "gate_unmet:" + why;
        if (!ok && !edge.host.dlg.empty()) {
            for (std::size_t i = 0; i < g.scene_.entities.size() && !ok; ++i) {
                const kotor::Entity& entity = g.scene_.entities[i];
                const std::uint32_t object = g.objectForEntity(i);
                if (entity.dead || lowered(entity.conversation) != edge.host.dlg || object == self) {
                    continue;
                }
                g.clearDialogueState();
                g.pendingModule_.clear();
                g.sim_->pendingLinkedTo.clear();
                std::string whyOther;
                bool ran = false;
                try {
                    ran = walkConversationTo(edge, object, whyOther);
                } catch (const std::exception& exception) {
                    whyOther = exception.what();
                }
                pumpAndDrain();
                ok = ran && t.effectHeld(edge.effect, object, before);
            }
        }
        // Queries with no state behind them: answered, and listed.
        if (!ok && !reqs.empty()) {
            QuestTraceOps::Established established;
            ReqSet calls;
            for (const Req& req : reqs) {
                if (req.kind == "call") {
                    calls.push_back(req);
                }
            }
            if (!calls.empty()) {
                t.establish(calls, self, established);
                if (!established.mocked.empty()) {
                    ok = attempt();
                    t.answers.clear();
                    if (ok) {
                        for (const std::string& name : established.mocked) {
                            note("answered", edge.id, "query " + name + " has no state to establish");
                        }
                    }
                }
            }
        }
        if (!ok && forceOnFailure && !edge.host.dlg.empty()) {
            // The node itself, without the walk: says the script works, not that it can be reached.
            g.clearDialogueState();
            t.runHost(edge.host, self, {});
            pumpAndDrain();
            ok = t.effectHeld(edge.effect, self, before);
            if (ok) {
                note("node_forced", edge.id, detail.empty() ? std::string("conversation route blocked") : detail);
            }
        }
        if (ok) {
            edgesDone.insert(edge.id);
        } else {
            ++edgesFailed;
        }
        if (replayOut != nullptr) {
            *replayOut << legIndex << '\t' << g.module_ << '\t' << (ok ? "ok" : "failed") << '\t' << edge.id << '\t' << detail << '\n';
        }
        if (g.activeDialogue_.has_value() && g.pendingModule_.empty()) {
            finishConversation();
        }
        return ok;
    }

    // A conversation left open: taken to its end by first replies, the way a player would leave it.
    void finishConversation()
    {
        for (int step = 0; step < 128 && g.dialogueView_.active && g.activeDialogue_.has_value(); ++step) {
            if (g.sim_->dialoguePaused && !waitOutPause()) {
                g.finishDialogue(true);
                return;
            }
            g.chooseDialogueOption(0);
        }
        if (g.activeDialogue_.has_value()) {
            g.finishDialogue(true);
        }
    }

    std::string stateDigest() const
    {
        std::ostringstream out;
        std::map<std::string, std::string> globals;
        for (const auto& [name, value] : g.scriptWorld_.globals) {
            std::string shown;
            if (value.type == script::ValueType::String) {
                shown = value.text.str();
            } else if (value.type == script::ValueType::Int) {
                if (value.integer == 0) {
                    continue; // Absent and zero read alike.
                }
                shown = std::to_string(value.integer);
            } else {
                continue;
            }
            if (!shown.empty()) {
                globals[lowered(name)] = std::move(shown);
            }
        }
        for (const auto& [name, shown] : globals) {
            out << name << '=' << shown << ';';
        }
        out << "|journal:";
        std::map<std::string, std::int32_t> journal;
        for (const auto& [plot, state] : g.journal_) {
            journal[lowered(plot)] = state;
        }
        for (const auto& [name, state] : journal) {
            out << name << '=' << state << ';';
        }
        out << "|module:" << lowered(g.module_) << "|xp:" << g.character_.xp << "|gold:" << g.gold_;
        out << "|party:";
        for (const std::string& tag : g.partyTags_) {
            out << lowered(tag) << ',';
        }
        return out.str();
    }

    // A save and a load of what it wrote must keep all plot state. The engine's own file format.
    bool saveAndReload(const std::filesystem::path& folder, std::string& difference)
    {
        ++saves;
        const std::string digest = stateDigest();
        const std::filesystem::path file = folder / ("replay_" + std::to_string(saves) + ".sav");
        if (!g.save(file) || !g.load(file)) {
            difference = "save_or_load_failed";
            ++saveMismatches;
            return false;
        }
        const std::string after = stateDigest();
        if (after != digest) {
            std::size_t at = 0;
            while (at < digest.size() && at < after.size() && digest[at] == after[at]) {
                ++at;
            }
            const std::size_t from = at > 24 ? at - 24 : 0;
            difference = digest.substr(from, 64) + " -> " + after.substr(from, 64);
            ++saveMismatches;
            return false;
        }
        return true;
    }

    // Enters a module the real way: the request the action makes, taken by a frame's update.
    bool takeTransition(const std::string& to)
    {
        finishConversation();
        for (int frame = 0; frame < 30 && lowered(g.module_) != lowered(to); ++frame) {
            InputState idle;
            g.update(0.05f, idle);
            if (g.dialogueView_.active && lowered(g.module_) != lowered(to) && g.pendingModule_.empty()) {
                finishConversation();
            }
        }
        const bool arrived = lowered(g.module_) == lowered(to);
        if (arrived) {
            // What OnEnter deferred happens before anyone can talk: k_pebn_pophawk destroys the placed
            // party member at 0.5 s and respawns them at 2 s. Left pending, it fired during the first
            // pump of a reply, and a closure on the old object was dropped with it.
            t.pump([]() { return false; });
        }
        return arrived;
    }

    bool returning = false; // On the way to a door's condition: no second such trip.

    bool crossLeg(const quest::ChainStep& leg, std::string& why, bool force)
    {
        if (leg.kind == "door") {
            const std::string tag = leg.via.substr(leg.via.find(':') + 1);
            // Several doors of a module can share a tag: this leg means the one linked to its target.
            std::size_t index = g.findEntity(tag);
            for (std::size_t i = 0; i < g.scene_.entities.size(); ++i) {
                const kotor::Entity& entity = g.scene_.entities[i];
                if (lowered(entity.tag) == lowered(tag) && lowered(entity.linkedModule) == lowered(leg.to)) {
                    index = i;
                    break;
                }
            }
            if (index == static_cast<std::size_t>(-1)) {
                why = "door_not_here:" + tag;
                return false;
            }
            note("no_walking", g.module_ + ":" + tag, "the door is used from where the party stands");
            UseOutcome outcome = g.useObjectImpl(index, false);
            if (outcome == UseOutcome::Locked) {
                const kotor::Entity& door = g.scene_.entities[index];
                if (door.keyRequired || door.plot) {
                    // Its OnFailToOpen has run: play what that asked for, then what the state it
                    // reads depends on, and try the door again. Nothing here opens it by hand.
                    pumpAndDrain();
                    if (lowered(g.module_) == lowered(leg.to) || takeTransition(leg.to)) {
                        return true;
                    }
                    if (g.scene_.entities[index].open) {
                        // Its own script opened it (k_ptar_alertsith): now it can be crossed.
                        g.useObjectImpl(index, false);
                        pumpAndDrain();
                        if (lowered(g.module_) == lowered(leg.to) || takeTransition(leg.to)) {
                            return true;
                        }
                    }
                    const auto hook = door.scripts.find("OnFailToOpen");
                    std::set<std::string> named;
                    if (hook != door.scripts.end()) {
                        if (const auto program = g.decodedScript(hook->second); program != nullptr) {
                            for (const script::Instruction& ins : program->code) {
                                if (ins.opcode == 0x04 && ins.qualifier == 0x05) {
                                    named.insert(lowered(std::string(ins.bytes.begin(), ins.bytes.end())));
                                }
                            }
                        }
                    }
                    const std::string from = lowered(g.module_);
                    bool played = false;
                    for (const Edge& edge : graph.edges) {
                        if (edge.module == from && edge.effect.kind == "global_set" && edge.effect.valueKnown && edge.effect.value != 0
                            && edge.dead.empty() && !edge.orphan && !edge.debug && named.contains(lowered(edge.effect.name))
                            && !edgesDone.contains(edge.id)) {
                            played = runEdge(edge, 0, false) || played;
                        }
                    }
                    // What names this door in the module can unlock it: the edges of a script that
                    // does (k_plev_finale5), and a trigger's enter script (k_ptar_canttrip).
                    const std::string wanted = lowered(tag);
                    const auto namesDoor = [&](const std::string& resref) {
                        const auto program = g.decodedScript(resref);
                        if (program == nullptr) {
                            return false;
                        }
                        return std::any_of(program->code.begin(), program->code.end(), [&](const script::Instruction& ins) {
                            return ins.opcode == 0x04 && ins.qualifier == 0x05
                                && lowered(std::string(ins.bytes.begin(), ins.bytes.end())) == wanted;
                        });
                    };
                    for (const Edge& edge : graph.edges) {
                        if (edge.module == from && edge.dead.empty() && !edge.orphan && !edge.debug && !edgesDone.contains(edge.id)
                            && namesDoor(edge.siteScript) && lowered(g.module_) == from) {
                            played = runEdge(edge, 0, false) || played;
                        }
                    }
                    for (std::size_t v = 0; v < g.scene_.triggers.size() && lowered(g.module_) == from; ++v) {
                        const kotor::Entity& trigger = g.scene_.entities[g.scene_.triggers[v].entity];
                        if (!trigger.scriptOnEnter.empty() && namesDoor(trigger.scriptOnEnter)) {
                            note("no_walking", g.module_ + ":" + trigger.tag, "the party is placed inside the trigger");
                            kotor::Vec3 centre{};
                            for (const kotor::Vec3& vertex : g.scene_.triggers[v].vertices) {
                                centre = centre + vertex;
                            }
                            g.position_ = centre * (1.0f / static_cast<float>(g.scene_.triggers[v].vertices.size()));
                            g.syncPlayerToScriptWorld();
                            if (v < g.triggerInside_.size()) {
                                g.triggerInside_[v] = false;
                            }
                            g.updateTriggers();
                            pumpAndDrain();
                            played = true;
                        }
                    }
                    if (g.scene_.entities[index].locked && !returning) {
                        const bool playedBefore = played;
                        played = false;
                        returning = true;
                        const std::string doorName = "tag:" + wanted;
                        const auto module = std::find_if(graph.modules.begin(), graph.modules.end(),
                                                         [&](const quest::ModuleInfo& info) { return info.name == from; });
                        std::vector<Req> wants;
                        if (module != graph.modules.end()) {
                            for (const auto& [resref, factsIndex] : module->scripts) {
                                for (const quest::Site& site : graph.facts[factsIndex].sites) {
                                    if (site.effect.kind == "door_op" && site.effect.aux == "SetLocked" && lowered(site.effect.name) == doorName
                                        && site.effect.valueKnown && site.effect.value == 0 && !site.paths.empty()) {
                                        for (const Req& req : site.paths.front()) {
                                            if (req.kind == "global" && !req.opaque()) {
                                                wants.push_back(req);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if (wants.empty() && module != graph.modules.end()) {
                            // The walk of a long area script may not have reached the call. The boolean read
                            // just before it says what to work towards; a wrong guess only fails to open.
                            for (const auto& [resref, factsIndex] : module->scripts) {
                                const auto program = namesDoor(resref) ? g.decodedScript(resref) : nullptr;
                                if (program == nullptr) {
                                    continue;
                                }
                                const std::vector<script::Instruction>& code = program->code;
                                for (std::size_t i = 4; i < code.size(); ++i) {
                                    const bool unlocks = code[i].opcode == 0x05 && std::string_view(script::actionSpec(code[i].actionId).name) == "SetLocked"
                                        && code[i - 4].opcode == 0x04 && code[i - 4].qualifier == 0x03 && code[i - 4].operand0 == 0
                                        && code[i - 2].opcode == 0x04 && code[i - 2].qualifier == 0x05
                                        && lowered(std::string(code[i - 2].bytes.begin(), code[i - 2].bytes.end())) == wanted;
                                    for (std::size_t back = 1; unlocks && back < 16 && back + 1 < i; ++back) {
                                        const script::Instruction& read = code[i - back];
                                        if (read.opcode == 0x05 && std::string_view(script::actionSpec(read.actionId).name) == "GetGlobalBoolean"
                                            && code[i - back - 1].opcode == 0x04 && code[i - back - 1].qualifier == 0x05) {
                                            Req req;
                                            req.kind = "global";
                                            req.name.assign(code[i - back - 1].bytes.begin(), code[i - back - 1].bytes.end());
                                            req.op = "true";
                                            wants.push_back(req);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        std::vector<std::string> routeFailures;
                        if (SDL_getenv("KOTOR_QUEST_DIAG") != nullptr) {
                            std::printf("plot replay diag: door %s wants %zu\n", tag.c_str(), wants.size());
                            for (const Req& req : wants) {
                                std::printf("  %s setters %zu\n", req.key().c_str(), settersFor(req).size());
                            }
                        }
                        for (const Req& req : wants) {
                            for (const Edge* setter : settersFor(req)) {
                                if (setter->module == "global" || setter->module == from) {
                                    continue;
                                }
                                goTo(setter->module, "door", routeFailures, false);
                                if (lowered(g.module_) == setter->module) {
                                    runEdge(*setter, 1, false);
                                    played = holds(req, g.scriptWorld_.pc).value_or(false);
                                }
                                goTo(from, "door", routeFailures, false);
                                break;
                            }
                        }
                        returning = false;
                        played = played || playedBefore;
                        if (lowered(g.module_) != from) {
                            why = "door_trip_lost:" + tag;
                            return false;
                        }
                        for (std::size_t i = 0; i < g.scene_.entities.size(); ++i) {
                            const kotor::Entity& entity = g.scene_.entities[i];
                            if (lowered(entity.tag) == lowered(tag) && lowered(entity.linkedModule) == lowered(leg.to)) {
                                index = i;
                                break;
                            }
                        }
                    }
                    if (played && lowered(g.module_) == from) {
                        g.useObjectImpl(index, false);
                        pumpAndDrain();
                        if (lowered(g.module_) == lowered(leg.to) || takeTransition(leg.to)) {
                            return true;
                        }
                    }
                    why = "door_locked:" + tag;
                    return false;
                }
                // Security and Bash are open to any player here (no key, not plot): not played.
                note("door_opened", g.module_ + ":" + tag, "the lock is not played; SetLocked(false) ran");
                std::vector<std::vector<script::Value>> args{{script::Value::makeObject(g.objectForEntity(index))}, {script::Value::makeInt(0)}};
                g.actionAsForTest(g.scriptWorld_.pc, 324, args); // retail 00543240
                outcome = g.useObjectImpl(index, false);
                if (outcome == UseOutcome::Locked) {
                    why = "door_locked:" + tag;
                    return false;
                }
            }
            if (lowered(g.module_) == lowered(leg.to) || takeTransition(leg.to)) {
                return true;
            }
            why = "door_no_transition:" + tag;
            return false;
        }
        if (leg.kind == "trigger") {
            const std::string tag = leg.via.substr(leg.via.find(':') + 1);
            std::size_t index = g.findEntity(tag);
            for (std::size_t i = 0; i < g.scene_.entities.size(); ++i) {
                const kotor::Entity& entity = g.scene_.entities[i];
                if (entity.kind == kotor::EntityKind::Trigger && lowered(entity.linkedModule) == lowered(leg.to)) {
                    index = i;
                    break;
                }
            }
            if (index == static_cast<std::size_t>(-1)) {
                why = "trigger_not_here:" + tag;
                return false;
            }
            kotor::Vec3 centre{};
            float count = 0.0f;
            std::size_t volumeIndex = SIZE_MAX;
            for (std::size_t i = 0; i < g.scene_.triggers.size(); ++i) {
                if (g.scene_.triggers[i].entity == index) {
                    volumeIndex = i;
                    for (const kotor::Vec3& vertex : g.scene_.triggers[i].vertices) {
                        centre = centre + vertex;
                        count += 1.0f;
                    }
                }
            }
            if (count == 0.0f) {
                why = "trigger_has_no_volume:" + tag;
                return false;
            }
            note("no_walking", g.module_ + ":" + tag, "the party is placed inside the trigger");
            g.position_ = centre * (1.0f / count);
            g.syncPlayerToScriptWorld();
            if (volumeIndex < g.triggerInside_.size()) {
                g.triggerInside_[volumeIndex] = false;
            }
            g.updateTriggers();
            pumpAndDrain();
            return takeTransition(leg.to);
        }
        const auto found = edgeById.find(leg.via);
        if (found == edgeById.end()) {
            why = "no_edge:" + leg.via;
            return false;
        }
        const Edge& edge = *found->second;
        edgesDone.erase(edge.id); // A way through can be used again: a trip goes out and comes back.
        if (!runEdge(edge, 0, force)) {
            why = "edge_failed:" + edge.id;
            return false;
        }
        return takeTransition(leg.to);
    }

    // ---- the route, chosen on the state as it stands ---------------------------------------

    const std::vector<quest::ModuleEdge>& hops() const { return graph.moduleEdges; }

    bool hopUsable(const quest::ModuleEdge& hop) const
    {
        const auto found = edgeById.find(hop.via);
        // A script nothing ships a caller for is a shortcut the static chain may take, not a route.
        return found == edgeById.end() || (!found->second->orphan && !found->second->debug && found->second->dead.empty());
    }

    // Distances over hops not already tried, so a shut hop gives way to a longer shipped route.
    std::map<std::string, std::size_t> distancesTo(const std::string& target, const std::set<std::string>& tried) const
    {
        std::map<std::string, std::size_t> distance{{target, 0}};
        std::vector<std::string> level{target};
        for (std::size_t at = 1; !level.empty(); ++at) {
            std::vector<std::string> next;
            for (const quest::ModuleEdge& hop : hops()) {
                if (hopUsable(hop) && !tried.contains(hop.from + ">" + hop.to + ">" + hop.via)
                    && std::find(level.begin(), level.end(), hop.to) != level.end() && distance.emplace(hop.from, at).second) {
                    next.push_back(hop.from);
                }
            }
            level = std::move(next);
        }
        return distance;
    }

    std::size_t hopScore(const quest::ModuleEdge& hop)
    {
        const auto found = edgeById.find(hop.via);
        if (found == edgeById.end()) {
            return 0;
        }
        bool fallback = false;
        const std::uint32_t self = t.ownerFor(found->second->host, fallback);
        return unmetCount(pickGate(*found->second, self), self) + (fallback ? 5 : 0);
    }

    // Walks to a module by shipped transitions, trying those whose conditions look best now.
    void goTo(const std::string& target, const char* label, std::vector<std::string>& failures, bool mayForce = true)
    {
        std::set<std::string> tried;
        for (int step = 0; step < 40 && lowered(g.module_) != target; ++step) {
            const std::string here = lowered(g.module_);
            if (here == "m12ab" && globalNow("ebo_turret_done") == 0) {
                // k_pebo_sthdeath* write this when the turret game's ships die, and no ship game is played.
                note("minigame", "m12ab:ebo_turret_done", "the turret game is not played; its result was written");
                std::vector<std::vector<script::Value>> args{{script::Value::makeString("ebo_turret_done")}, {script::Value::makeInt(1)}};
                g.actionAsForTest(g.scriptWorld_.pc, QuestTraceOps::kSetGlobalBoolean, args);
            }
            std::vector<std::pair<std::size_t, const quest::ModuleEdge*>> options;
            const auto gatherOptions = [&]() {
                options.clear();
                const std::map<std::string, std::size_t> distance = distancesTo(target, tried);
                const auto mine = distance.find(here);
                if (mine != distance.end()) {
                    for (const quest::ModuleEdge& hop : hops()) {
                        const auto next = distance.find(hop.to);
                        if (hop.from == here && hopUsable(hop) && next != distance.end() && next->second < mine->second
                            && !tried.contains(hop.from + ">" + hop.to + ">" + hop.via)) {
                            options.emplace_back((next->second * 16) + hopScore(hop), &hop);
                        }
                    }
                }
                std::stable_sort(options.begin(), options.end(),
                                 [](const auto& a, const auto& b) { return a.first < b.first; });
            };
            gatherOptions();
            // Forcing, if it comes to that, is of the best route as it first stood.
            const std::vector<std::pair<std::size_t, const quest::ModuleEdge*>> firstOptions = options;
            bool crossed = false;
            std::string why = options.empty() ? std::string("no_shipped_route") : std::string{};
            const std::string destination = options.empty() ? target : options.front().second->to;
            for (const bool force : {false, true}) {
                if (force && !mayForce) {
                    break;
                }
                if (force) {
                    options = firstOptions;
                }
                for (std::size_t i = 0; i < options.size() && i < 8 && !crossed; ++i) {
                    const quest::ModuleEdge& hop = *options[i].second;
                    if (lowered(g.module_) != here) {
                        break;
                    }
                    ++legIndex;
                    tried.insert(hop.from + ">" + hop.to + ">" + hop.via);
                    std::string reason;
                    quest::ChainStep leg;
                    leg.from = hop.from;
                    leg.to = hop.to;
                    leg.via = hop.via;
                    leg.kind = hop.kind;
                    crossed = crossLeg(leg, reason, force);
                    if (replayOut != nullptr) {
                        *replayOut << legIndex << '\t' << g.module_ << '\t' << (crossed ? "crossed" : "tried") << '\t' << hop.via << '\t' << reason << '\n';
                    }
                    if (!crossed && force && why.empty()) {
                        why = reason;
                    }
                    if (crossed) {
                        ++legsDone;
                        if (onState) {
                            onState();
                        }
                        std::printf("plot replay: %s %s -> %s via %s t=%.0fs peak=%zuMB\n", label, hop.from.c_str(), hop.to.c_str(),
                                    hop.via.substr(0, 96).c_str(), seconds(), peakMemoryMb());
                        std::fflush(stdout);
                    }
                    // Every hop of this length shut: look again, without them, for a longer way round.
                    if (!force && !crossed && i + 1 == std::min<std::size_t>(options.size(), 8)) {
                        gatherOptions();
                        i = static_cast<std::size_t>(-1);
                    }
                }
                if (crossed || lowered(g.module_) != here) {
                    break;
                }
            }
            if (!crossed && lowered(g.module_) == here && !mayForce) {
                return;
            }
            if (!crossed && lowered(g.module_) == here) {
                const std::string subject = here + "->" + destination;
                note("leg_forced", subject, why);
                std::printf("plot replay: %s %s FORCED: %s\n", label, subject.c_str(), why.substr(0, 120).c_str());
                g.pendingModule_.clear();
                g.clearDialogueState();
                g.requestModule(destination);
                takeTransition(destination);
                ++legsDone;
            }
        }
        if (lowered(g.module_) != target) {
            failures.push_back("route_stopped\t" + std::string(label) + ":" + lowered(g.module_) + "->" + target);
        }
    }

    // What the plot wants done at a stop before it moves on: its master counters, written by shipped
    // edges whose every condition can be read. Others of the same script run with them.
    void plotWorkAt(const std::string& module, std::vector<std::string>& failures)
    {
        static const std::set<std::string> kMaster{"k_kotor_master", "k_star_map", "k_captured_lev"};
        std::set<std::string> done;
        for (const bool orphans : {false, true}) {
            for (const Edge& edge : graph.edges) {
                const std::string name = lowered(edge.effect.name);
                if (edge.module != module || edge.effect.kind != "global_set" || !kMaster.contains(name) || edge.debug
                    || !edge.dead.empty() || edge.orphan != orphans || done.contains(name)) {
                    continue;
                }
                bool fallback = false;
                const std::uint32_t self = t.ownerFor(edge.host, fallback);
                const ReqSet& reqs = pickGate(edge, self);
                if (std::any_of(reqs.begin(), reqs.end(), [](const Req& req) { return req.opaque(); })
                    || unmetCount(reqs, self) != 0) {
                    continue; // Conditional on more than this stop knows; the shipped script decides.
                }
                if (orphans) {
                    note("script_direct", edge.id, "no shipped caller is known; the script ran directly");
                }
                if (runEdge(edge, 0, true)) {
                    done.insert(name);
                } else {
                    failures.push_back("setter_failed\t" + edge.id);
                }
                if (lowered(g.module_) != module) {
                    return;
                }
            }
        }
    }

    bool saveHere(const std::filesystem::path& folder, const char* label, std::vector<std::string>& failures)
    {
        std::string difference;
        if (!saveAndReload(folder, difference)) {
            failures.push_back("save_reload\t" + std::string(label) + ":" + lowered(g.module_) + ":" + difference);
            return false;
        }
        return true;
    }

    std::size_t legsDone = 0;
    std::string dialoguesOf;
    std::function<void()> onState;
};

namespace quest {

int runConversationFlow(GameSession& game, const FlowOptions& options)
{
    std::error_code error;
    std::filesystem::create_directories(options.outDir, error);
    std::vector<QuestFlowOps::PlotState> plotStates;
    if (options.breakName.empty()) {
        // The plot replay first, on the session as it starts, for the states it reaches. Its result
        // is that gate's business; here it only supplies states.
        const std::filesystem::path start = std::filesystem::path(options.outDir) / "flow_start.sav";
        if (game.save(start)) {
            FlowOptions replay;
            replay.outDir = (std::filesystem::path(options.outDir) / "seed_replay").string();
            replay.deadlineSeconds = 180.0;
            replay.onPlotState = [&]() {
                plotStates.push_back({lowered(game.module_), game.scriptWorld_.globals, game.journal_});
            };
            runPlotReplay(game, replay);
            game.load(start);
        }
    }
    kotor::Install install(game.installPath(), false);
    install.addPacks();
    const Graph graph = buildGraph(install);
    QuestFlowOps ops(game, graph);
    ops.t.installAnswers();
    if (options.breakName == "globals") {
        ops.breakGlobals();
    }
    const FlowBaseline baseline = readFlowBaseline(options.baseline);
    std::vector<std::string> modules = splitCommas(options.modules);
    if (modules.empty()) {
        modules.assign(graph.mainPath.begin(), graph.mainPath.end());
    }
    std::ofstream out(std::filesystem::path(options.outDir) / "conversation_flow.tsv", std::ios::binary);
    out << "module\tdialogue\tpath\tnode\n";
    QuestFlowOps::FlowCounts counts;
    std::vector<std::pair<std::string, std::string>> failures;
    std::set<std::string> done;
    std::size_t modulesDone = 0;
    std::size_t skippedForTime = 0;
    for (const std::string& name : modules) {
        if (ops.seconds() > options.deadlineSeconds) {
            ++skippedForTime;
            continue;
        }
        std::vector<const Edge*> edges;
        for (const Edge& edge : graph.edges) {
            if (edge.module == name) {
                edges.push_back(&edge);
            }
        }
        const auto found = std::find_if(graph.modules.begin(), graph.modules.end(),
                                        [&](const ModuleInfo& module) { return module.name == name; });
        ops.t.module = found != graph.modules.end() ? &*found : nullptr;
        std::string loadError;
        if (!ops.t.load(name, edges, loadError)) {
            failures.emplace_back("module_load", name + ":" + loadError.substr(0, 80));
            continue;
        }
        const std::vector<kotor::Entity> entities = game.scene_.entities;
        std::map<std::string, std::size_t> owners;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            if (!entities[i].dead && !entities[i].conversation.empty()) {
                owners.emplace(lowered(entities[i].conversation), i);
            }
        }
        for (const auto& [resref, index] : owners) {
            if (ops.seconds() > options.deadlineSeconds) {
                break;
            }
            if (done.insert(resref).second) {
                ops.walkDialogue(name, resref, index, edges, counts, failures, out);
            }
        }
        ++modulesDone;
        ops.t.restore();
        std::printf("conversation flow: %s dialogues=%zu paths=%zu t=%.0fs peak=%zuMB\n", name.c_str(), counts.dialogues,
                    counts.paths, ops.seconds(), peakMemoryMb());
        std::fflush(stdout);
    }
    // Again on the plot's own states, for what a blank state left unseen: the states the replay had
    // on reaching this module, then a few from across the whole plot.
    std::size_t seedSkipped = 0;
    if (!plotStates.empty()) {
        const double seedDeadline = options.deadlineSeconds * 0.85;
        for (const std::string& name : modules) {
            if (ops.seconds() > seedDeadline) {
                ++seedSkipped;
                continue;
            }
            std::vector<const Edge*> edges;
            for (const Edge& edge : graph.edges) {
                if (edge.module == name) {
                    edges.push_back(&edge);
                }
            }
            const auto found = std::find_if(graph.modules.begin(), graph.modules.end(),
                                            [&](const ModuleInfo& module) { return module.name == name; });
            ops.t.module = found != graph.modules.end() ? &*found : nullptr;
            std::string loadError;
            if (!ops.t.load(name, edges, loadError)) {
                continue;
            }
            std::vector<const QuestFlowOps::PlotState*> seeds;
            const QuestFlowOps::PlotState* first = nullptr;
            const QuestFlowOps::PlotState* last = nullptr;
            for (const QuestFlowOps::PlotState& state : plotStates) {
                if (state.module == name) {
                    first = first == nullptr ? &state : first;
                    last = &state;
                }
            }
            if (first != nullptr) {
                seeds.push_back(first);
            }
            if (last != nullptr && last != first) {
                seeds.push_back(last);
            }
            for (const std::size_t part : {std::size_t{4}, std::size_t{2}, std::size_t{1}}) {
                seeds.push_back(&plotStates[(plotStates.size() - 1) / part]);
            }
            std::map<std::string, std::size_t> owners;
            for (std::size_t i = 0; i < game.scene_.entities.size(); ++i) {
                const kotor::Entity& entity = game.scene_.entities[i];
                if (!entity.dead && !entity.conversation.empty()) {
                    owners.emplace(lowered(entity.conversation), i);
                }
            }
            for (const auto& [resref, index] : owners) {
                const quest::DlgInfo* info = ops.dialogueInfo(name, resref);
                for (const QuestFlowOps::PlotState* seed : seeds) {
                    if (ops.seconds() > seedDeadline || info == nullptr
                        || ops.flowSeen[resref].size() >= info->entries.size() + info->replies.size()) {
                        break;
                    }
                    ops.flowSeed = seed;
                    ops.walkDialogue(name, resref, index, edges, counts, failures, out);
                    ops.flowSeed = nullptr;
                }
                // Then each link still shut, with its own condition's state written true. This shows the
                // nodes behind it work, not that the plot can make that state: counted apart.
                if (info == nullptr || found == graph.modules.end()) {
                    continue;
                }
                std::set<std::string> triedConditions;
                for (int round = 0; round < 24 && ops.seconds() < seedDeadline; ++round) {
                    const auto& seen = ops.flowSeen[resref];
                    const quest::ScriptFacts* facts = nullptr;
                    const auto consider = [&](const std::string& active) {
                        const std::string script = lowered(active);
                        if (facts != nullptr || script.empty() || triedConditions.contains(script)) {
                            return;
                        }
                        const quest::ScriptFacts* candidate = nullptr;
                        if (const auto own = found->scripts.find(script); own != found->scripts.end()) {
                            candidate = &graph.facts[own->second];
                        } else if (const auto global = graph.globalScripts.find(script); global != graph.globalScripts.end()) {
                            candidate = &graph.facts[global->second];
                        }
                        if (candidate != nullptr && !candidate->truePaths.empty() && !candidate->truePaths.front().empty()) {
                            triedConditions.insert(script);
                            facts = candidate;
                        }
                    };
                    for (const auto& [start, active] : info->starting) {
                        if (!seen.contains({false, start})) {
                            consider(active);
                        }
                    }
                    for (const auto& [isReply, node] : seen) {
                        const auto& nodes = isReply ? info->replies : info->entries;
                        if (node < nodes.size()) {
                            for (const auto& [to, active] : nodes[node].links) {
                                if (!seen.contains({!isReply, to})) {
                                    consider(active);
                                }
                            }
                        }
                    }
                    if (facts == nullptr) {
                        break;
                    }
                    ops.flowReqs = &facts->truePaths.front();
                    ops.walkDialogue(name, resref, index, edges, counts, failures, out);
                    ops.flowReqs = nullptr;
                }
            }
            ops.t.restore();
        }
    }
    std::vector<std::string> unexpected;
    std::size_t excepted = 0;
    for (const auto& [kind, subject] : failures) {
        const auto exception = baseline.exceptions.find(kind + "|" + subject);
        if (exception != baseline.exceptions.end()) {
            ++excepted;
            std::printf("  exception %s %s: %s\n", kind.c_str(), subject.c_str(), exception->second.c_str());
        } else {
            unexpected.push_back(kind + "\t" + subject);
        }
    }
    for (std::size_t i = 0; i < unexpected.size() && i < 60; ++i) {
        std::printf("  FAIL %s\n", unexpected[i].c_str());
    }
    std::printf("conversation flow: modules=%zu dialogues=%zu paths=%zu nodes_seen=%zu of %zu blocked_at_start=%zu "
                "effects_checked=%zu loops_cut=%zu paused_out=%zu left_module=%zu skipped_for_time=%zu\n",
                modulesDone, counts.dialogues, counts.paths, counts.nodesSeen, counts.nodesTotal, counts.blockedAtStart,
                counts.effectsChecked, counts.loopsCut, counts.pausedOut, counts.moduleLeft, skippedForTime);
    const bool ok = unexpected.empty() && skippedForTime == 0 && counts.effectsChecked > 0;
    std::printf("conversation flow: plot_states=%zu nodes_seen_on_plot_states=%zu nodes_seen_by_condition_state=%zu total_seen=%zu of %zu seed_runs=%zu modules_without_seed_pass=%zu\n",
                plotStates.size(), counts.nodesSeeded, counts.nodesByCondition, counts.nodesSeen + counts.nodesSeeded + counts.nodesByCondition,
                counts.nodesTotal, counts.seedRuns, seedSkipped);
    std::printf("conversation flow gate: %s fail=%zu excepted=%zu time=%.0fs peak=%zuMB%s\n", ok ? "PASS" : "FAIL",
                unexpected.size(), excepted, ops.seconds(), peakMemoryMb(),
                options.breakName.empty() ? "" : " (deliberately broken run)");
    return ok ? 0 : 1;
}

int runPlotReplay(GameSession& game, const FlowOptions& options)
{
    std::error_code error;
    std::filesystem::create_directories(options.outDir, error);
    kotor::Install install(game.installPath(), false);
    install.addPacks();
    const Graph graph = buildGraph(install);
    QuestFlowOps ops(game, graph);
    ops.t.installAnswers();
    ops.indexSetters();
    ops.dropSetter = lowered(options.breakName);
    ops.onState = options.onPlotState;
    const FlowBaseline baseline = readFlowBaseline(options.baseline);
    std::ofstream out(std::filesystem::path(options.outDir) / "plot_replay.tsv", std::ios::binary);
    out << "leg\tmodule\tstatus\tedge\tdetail\n";
    ops.replayOut = &out;

    ops.note("no_chargen", "new game", "the default character of the session stands in for character creation");
    if (!ops.dropSetter.empty()) {
        ops.dropGlobalWrites(ops.dropSetter);
    }

    std::vector<std::string> legFailures;
    const std::filesystem::path outDir(options.outDir);
    const auto over = [&]() { return ops.seconds() > options.deadlineSeconds; };
    // The session's first frames run the first module's entry scripts on the untouched state.
    for (int frame = 0; frame < 4; ++frame) {
        InputState idle;
        game.update(0.05f, idle);
    }
    ops.finishConversation();

    // The final choice is taken in unk_m44ac (k_punk_bastesc, k_punk_bastjoin read as 2 and 1 by the
    // star forge's own branch, k_psta_bastleav2): shared up to there, one save, then each ending.
    const std::string forkModule = "unk_m44ac";
    const auto stops = [&](const char* label, const std::vector<std::string>& modules) {
        for (const std::string& stop : modules) {
            if (over()) {
                legFailures.push_back(std::string("time_budget\t") + label + ":" + stop);
                return;
            }
            ops.goTo(stop, label, legFailures);
            if (lowered(game.module_) == stop) {
                ops.plotWorkAt(stop, legFailures);
                if (lowered(game.module_) == stop) {
                    ops.saveHere(outDir, label, legFailures);
                }
            }
        }
    };
    std::vector<std::string> shared;
    for (const std::string& stop : graph.checkpoints) {
        if (stop != graph.checkpoints.front()) {
            shared.push_back(stop);
        }
        if (stop == forkModule) {
            break;
        }
    }
    stops("shared", shared);
    const std::filesystem::path forkSave = outDir / "replay_fork.sav";
    const bool forkSaved = game.save(forkSave);
    const auto ending = [&](const char* label, std::int64_t choice, std::vector<std::string> modules) {
        const Edge* chosen = nullptr;
        for (const Edge& edge : graph.edges) {
            if (edge.module == forkModule && edge.effect.kind == "global_set" && lowered(edge.effect.name) == "g_finalchoice"
                && edge.effect.valueKnown && edge.effect.value == choice && !edge.debug) {
                chosen = &edge;
                break;
            }
        }
        if (chosen == nullptr || !ops.runEdge(*chosen, 0, true)) {
            legFailures.push_back(std::string("final_choice\t") + label);
        }
        ops.edgesDone.clear();
        stops(label, modules);
        return game.module_;
    };
    const std::string lightEnd = ending("light", 2, {"sta_m45aa", "stunt_56a", "stunt_57"});
    std::string darkEnd;
    if (forkSaved && game.load(forkSave)) {
        ops.saves += 1;
        darkEnd = ending("dark", 1, {"sta_m45aa", "stunt_54a", "stunt_55a"});
    } else {
        legFailures.push_back("save_reload\tfork");
    }
    const bool endingsReached = std::find(graph.endingModules.begin(), graph.endingModules.end(), lowered(lightEnd)) != graph.endingModules.end()
        && std::find(graph.endingModules.begin(), graph.endingModules.end(), lowered(darkEnd)) != graph.endingModules.end();
    if (!endingsReached) {
        legFailures.push_back("ending_not_reached\t" + lightEnd + "," + darkEnd);
    }
    const std::size_t legsDone = ops.legsDone;
    const std::size_t fork = shared.size();

    // Kinds a player's route stands behind: not played, listed, and no failure in themselves.
    static const std::set<std::string> kNotPlayed{"no_chargen", "no_walking", "kill", "answered", "door_opened", "minigame", "party_panel"};
    std::vector<std::string> unexpected = legFailures;
    std::ofstream bypassOut(outDir / "plot_replay_bypasses.tsv", std::ios::binary);
    bypassOut << "kind\tsubject\treason\n";
    std::map<std::string, std::size_t> kinds;
    std::size_t excepted = 0;
    for (const QuestFlowOps::Bypass& bypass : ops.bypasses) {
        bypassOut << bypass.kind << '\t' << bypass.subject << '\t' << bypass.reason << '\n';
        ++kinds[bypass.kind];
        std::printf("  bypass %s %s: %s\n", bypass.kind.c_str(), bypass.subject.c_str(), bypass.reason.c_str());
        if (kNotPlayed.contains(bypass.kind)) {
            continue;
        }
        if (baseline.exceptions.contains(bypass.kind + "|" + bypass.subject)) {
            ++excepted;
        } else {
            unexpected.push_back(bypass.kind + "\t" + bypass.subject + "\t" + bypass.reason);
        }
    }
    std::vector<std::string> kept;
    for (const std::string& line : unexpected) {
        const std::size_t tab = line.find('\t');
        const std::size_t tab2 = line.find('\t', tab + 1);
        const std::string key = line.substr(0, tab) + "|" + line.substr(tab + 1, tab2 == std::string::npos ? std::string::npos : tab2 - tab - 1);
        if (baseline.exceptions.contains(key)) {
            ++excepted;
        } else {
            kept.push_back(line);
        }
    }
    for (std::size_t i = 0; i < kept.size() && i < 80; ++i) {
        std::printf("  FAIL %s\n", kept[i].c_str());
    }
    for (const auto& [kind, count] : kinds) {
        std::printf("  bypass count %-20s %zu\n", kind.c_str(), count);
    }
    std::printf("plot replay: legs=%zu fork=%zu edges_run=%zu edges_failed=%zu saves=%zu save_mismatches=%zu endings=%s,%s\n",
                legsDone, fork, ops.edgesRun, ops.edgesFailed, ops.saves, ops.saveMismatches, lightEnd.c_str(), darkEnd.c_str());
    const bool ok = kept.empty() && legsDone > 0;
    ops.t.uninstallAnswers();
    std::printf("plot replay gate: %s fail=%zu excepted=%zu time=%.0fs peak=%zuMB%s\n", ok ? "PASS" : "FAIL", kept.size(), excepted,
                ops.seconds(), peakMemoryMb(), options.breakName.empty() ? "" : " (deliberately broken run)");
    return ok ? 0 : 1;
}

} // namespace quest

} // namespace ke
