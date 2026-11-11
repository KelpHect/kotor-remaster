// Stages 2 and 3 of the quest tracer and its gate: docs/recon/64_QUEST_TRACE.md.
//
// WHAT THIS IS NOT. This mode writes journal, global, local and inventory state directly
// into the session before it runs a script. A harness write of that kind would be
// cheating in a playthrough; it is allowed here only because this mode proves that the
// shipped script and the engine underneath it do the right thing once the recorded
// preconditions hold. It does not prove a player can reach those preconditions, win the
// fights on the way, or that timing and state hold over a real run.
//
// A gate on an engine query with no state behind it (GetIsPC, Random, GetTag) is met by
// answering that query for one run. Those passes prove the script's branch and its effect,
// not the query; each carries a "mock:" reason so they are counted apart from clean passes.
#include "game/QuestTraceOps.h"

namespace ke {

namespace quest {

namespace {

struct Baseline {
    bool loaded = false;
    std::int64_t passFloor = 0;
    std::map<std::string, std::string> edgeExceptions;
    std::map<std::string, std::string> navExceptions; // "module|tag"
};

Baseline readBaseline(const std::string& path)
{
    Baseline out;
    if (path.empty()) {
        return out;
    }
    std::ifstream file(path);
    if (!file) {
        return out;
    }
    out.loaded = true;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::vector<std::string> fields;
        std::stringstream stream(line);
        std::string field;
        while (std::getline(stream, field, '\t')) {
            fields.push_back(field);
        }
        if (fields.size() >= 2 && fields[0] == "pass_floor") {
            out.passFloor = std::stoll(fields[1]);
        } else if (fields.size() >= 3 && fields[0] == "edge") {
            out.edgeExceptions[fields[1]] = fields[2];
        } else if (fields.size() >= 4 && fields[0] == "nav") {
            out.navExceptions[fields[1] + "|" + fields[2]] = fields[3];
        }
    }
    return out;
}

std::vector<std::string> splitList(const std::string& text)
{
    std::vector<std::string> out;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            out.push_back(lowered(item));
        }
    }
    return out;
}

// A test-only fault for the negative check: AddJournalQuestEntry does nothing.
std::optional<std::vector<script::Value>> brokenJournalAction(ActionCall&)
{
    return std::vector<script::Value>{};
}

} // namespace

int runQuestTrace(GameSession& game, const QuestTraceOptions& options)
{
    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    const auto seconds = [&]() {
        return std::chrono::duration<double>(clock::now() - started).count();
    };
    std::error_code error;
    std::filesystem::create_directories(options.outDir, error);

    // Stage 1. A second, unbound view of the install: the session's is bound to a module.
    kotor::Install install(game.installPath(), false);
    install.addPacks();
    const Graph graph = buildGraph(install);
    writeGraphJson(graph, std::filesystem::path(options.outDir) / "quest_graph.json");
    std::size_t entries = 0;
    for (const JournalQuest& quest : graph.quests) {
        entries += quest.entries.size();
    }
    std::printf("quest graph: modules=%zu quests=%zu entries=%zu edges=%zu scripts=%zu truncated=%zu "
                "decode_failed=%zu dialogues=%zu nodes=%zu unresolved=%zu no_setter=%zu\n",
                graph.modules.size(), graph.quests.size(), entries, graph.edges.size(), graph.scriptCount,
                graph.scriptsTruncated, graph.scriptsFailed, graph.dlgCount, graph.nodeCount, graph.unresolved.size(),
                graph.entriesNoSetter.size());
    std::printf("quest graph: %.1fs peak=%zuMB\n", seconds(), peakMemoryMb());
    if (options.graphOnly) {
        return 0;
    }

    // Stage 2 and 3, one module at a time.
    const std::vector<std::string> only = splitList(options.modules);
    std::map<std::string, std::vector<const Edge*>> byModule;
    for (const Edge& edge : graph.edges) {
        const std::string name = edge.module == "global" ? std::string("end_m01aa") : edge.module;
        if (only.empty() || std::find(only.begin(), only.end(), name) != only.end()) {
            byModule[name].push_back(&edge);
        }
    }

    QuestTraceOps ops{game, graph};
    ops.installAnswers();
    std::printf("quest graph: run variable walks=%zu cut=%zu\n", graph.runVarWalks, graph.runVarWalksCut);
    std::ofstream failLog(std::filesystem::path(options.outDir) / "quest_fail.log", std::ios::binary);
    ops.failLog = &failLog;
    if (options.breakName == "journal") {
        game.sim_->handlers.add(367, brokenJournalAction);
    }
    std::map<std::string, EdgeResult> results;
    std::vector<NavRow> navRows;
    std::size_t modulesDone = 0;
    for (const auto& [name, edges] : byModule) {
        const auto found = std::find_if(graph.modules.begin(), graph.modules.end(),
                                        [&, moduleName = name](const ModuleInfo& module) { return module.name == moduleName; });
        ops.module = found != graph.modules.end() ? &*found : nullptr;
        const double moduleStart = seconds();
        if (seconds() > options.deadlineSeconds) {
            for (const Edge* edge : edges) {
                results[edge->id] = {Status::Cant, "time_budget"};
            }
            continue;
        }
        std::string loadError;
        ops.timeRestore = ops.timeRun = ops.timePump = 0.0;
        // KOTOR_QUEST_DIAG: which stage of a module grew the peak.
        const bool diag = SDL_getenv("KOTOR_QUEST_DIAG") != nullptr;
        std::size_t diagPeak = peakMemoryMb();
        const auto stage = [&](const char* what, const std::string& detail) {
            if (diag && peakMemoryMb() > diagPeak + 100) {
                std::printf("quest trace diag: %s %s %s peak %zu -> %zuMB t=%.1fs\n", name.c_str(), what, detail.c_str(), diagPeak,
                            peakMemoryMb(), seconds() - moduleStart);
                diagPeak = peakMemoryMb();
            }
        };
        if (!ops.load(name, edges, loadError)) {
            for (const Edge* edge : edges) {
                results[edge->id] = {Status::Fail, "module_load:" + loadError.substr(0, 80)};
            }
            continue;
        }
        // Edges that share a host and a first gate set run together.
        std::map<std::string, std::vector<const Edge*>> groups;
        for (const Edge* edge : edges) {
            if (!edge->dead.empty()) {
                results[edge->id] = {Status::Dead, edge->dead};
                continue;
            }
            std::string key = edge->host.id() + "#";
            if (!edge->gates.empty()) {
                for (const Req& req : edge->gates.front()) {
                    key += req.key() + ";";
                }
            }
            groups[key].push_back(edge);
        }
        for (const auto& [key, group] : groups) {
            std::size_t most = 1;
            for (const Edge* edge : group) {
                most = std::max(most, edge->gates.size());
            }
            for (std::size_t alternative = 0; alternative < most; ++alternative) {
                std::vector<const Edge*> open;
                for (const Edge* edge : group) {
                    const auto result = results.find(edge->id);
                    if ((result == results.end() || result->second.status != Status::Pass)
                        && alternative < std::max<std::size_t>(1, edge->gates.size())) {
                        open.push_back(edge);
                    }
                }
                if (open.empty()) {
                    break;
                }
                ops.runGroup(open, alternative, results);
                stage("group", key.substr(0, 90));
            }
        }
        stage("load+groups", "");
        ops.sweep(edges, results, [&]() { return seconds() > options.deadlineSeconds; });
        stage("sweep", "");
        ops.clearAfterRun();
        ops.navStage(edges, navRows);
        stage("nav", "");
        ++modulesDone;
        std::printf("quest trace: %s edges=%zu groups=%zu t=%.1fs restore=%.1f run=%.1f pump=%.1f total=%.0fs peak=%zuMB\n",
                    name.c_str(), edges.size(), groups.size(), seconds() - moduleStart, ops.timeRestore, ops.timeRun,
                    ops.timePump, seconds(), peakMemoryMb());
        std::fflush(stdout);
    }

    // ---- the report ---------------------------------------------------------------------

    const Baseline baseline = readBaseline(options.baseline);
    std::map<std::string, std::array<std::size_t, 4>> perPlanet;
    std::map<std::string, std::size_t> reasons;
    std::map<std::string, std::array<std::size_t, 5>> perKind;
    std::size_t passMocked = 0;
    std::array<std::size_t, 4> totals{};
    std::vector<std::string> newFailures;
    std::vector<std::string> staleExceptions;
    std::ofstream edgesOut(std::filesystem::path(options.outDir) / "quest_trace.tsv", std::ios::binary);
    edgesOut << "status\tplanet\tedge\treason\n";
    for (const Edge& edge : graph.edges) {
        const auto found = results.find(edge.id);
        if (found == results.end()) {
            continue;
        }
        const EdgeResult& result = found->second;
        const std::size_t slot = static_cast<std::size_t>(result.status);
        ++totals[slot];
        ++perPlanet[edge.planet][slot];
        ++perKind[edge.effect.kind][slot];
        if (result.status == Status::Pass && !result.reason.empty()) {
            ++passMocked;
            ++perKind[edge.effect.kind][4];
        }
        edgesOut << statusText(result.status) << '\t' << edge.planet << '\t' << edge.id << '\t' << result.reason << '\n';
        if (result.status != Status::Pass) {
            const std::string family = result.reason.substr(0, result.reason.find(':'));
            ++reasons[std::string(statusText(result.status)) + ":" + family];
        }
        const bool excepted = baseline.edgeExceptions.contains(edge.id);
        if (result.status == Status::Fail && !excepted) {
            newFailures.push_back(edge.id + "\t" + result.reason);
        }
        if (result.status == Status::Pass && excepted) {
            staleExceptions.push_back(edge.id);
        }
    }
    edgesOut.close();

    std::vector<std::string> newUnreachable;
    std::map<std::string, std::size_t> navCounts;
    std::ofstream navOut(std::filesystem::path(options.outDir) / "quest_nav.tsv", std::ios::binary);
    navOut << "status\tmain_path\tmodule\tsubject\tkind\tfrom\tdoors\n";
    for (const NavRow& row : navRows) {
        navOut << row.status << '\t' << (row.mainPath ? 1 : 0) << '\t' << row.module << '\t' << row.subject << '\t'
               << row.kind << '\t' << row.from << '\t' << row.doors << '\n';
        ++navCounts[(row.mainPath ? "main:" : "side:") + row.status];
        if (row.mainPath && (row.status == "unreachable" || row.status == "door_no_opener")
            && !baseline.navExceptions.contains(row.module + "|" + row.subject)) {
            newUnreachable.push_back(row.module + "|" + row.subject + "|" + row.status);
        }
    }
    navOut.close();

    std::printf("quest trace: modules=%zu edges pass=%zu (of which answered queries=%zu) fail=%zu cant=%zu dead=%zu\n", modulesDone,
                totals[0], passMocked, totals[1], totals[2], totals[3]);
    for (const auto& [kind, counts] : perKind) {
        std::printf("  effect %-16s pass=%zu (answered=%zu) fail=%zu cant=%zu dead=%zu\n", kind.c_str(), counts[0], counts[4], counts[1],
                    counts[2], counts[3]);
    }
    for (const auto& [planet, counts] : perPlanet) {
        std::printf("  planet %-12s pass=%zu fail=%zu cant=%zu dead=%zu\n", planet.c_str(), counts[0], counts[1], counts[2],
                    counts[3]);
    }
    for (const auto& [reason, count] : reasons) {
        std::printf("  reason %-40s %zu\n", reason.c_str(), count);
    }
    for (const auto& [name, count] : navCounts) {
        std::printf("  nav %-32s %zu\n", name.c_str(), count);
    }
    std::printf("  chain: %s endings=%zu light_steps=%zu dark_steps=%zu main_path_modules=%zu missing_legs=%zu "
                "gates_no_setter=%zu\n",
                graph.chainResult.c_str(), graph.endingModules.size(), graph.chainLight.size(),
                graph.chainDark.size(), graph.mainPath.size(), graph.chainMissingLegs.size(),
                graph.chainGatesNoSetter.size());
    for (const std::string& leg : graph.chainMissingLegs) {
        std::printf("  chain missing leg %s\n", leg.c_str());
    }
    for (std::size_t i = 0; i < newFailures.size() && i < 40; ++i) {
        std::printf("  FAIL %s\n", newFailures[i].c_str());
    }
    for (std::size_t i = 0; i < newUnreachable.size() && i < 40; ++i) {
        std::printf("  UNREACHABLE %s\n", newUnreachable[i].c_str());
    }
    for (std::size_t i = 0; i < staleExceptions.size() && i < 20; ++i) {
        std::printf("  stale exception (now passes): %s\n", staleExceptions[i].c_str());
    }

    const bool partial = !only.empty();
    const bool passFloorOk = partial || !baseline.loaded || static_cast<std::int64_t>(totals[0]) >= baseline.passFloor;
    if (!passFloorOk) {
        std::printf("  pass count %zu fell below the baseline floor %lld\n", totals[0],
                    static_cast<long long>(baseline.passFloor));
    }
    const bool ok = newFailures.empty() && newUnreachable.empty() && passFloorOk;
    std::printf("quest trace gate: %s engine_fail=%zu unreachable=%zu time=%.0fs peak=%zuMB%s\n", ok ? "PASS" : "FAIL",
                newFailures.size(), newUnreachable.size(), seconds(), peakMemoryMb(),
                options.breakName.empty() ? "" : " (deliberately broken run)");
    return ok ? 0 : 1;
}

} // namespace quest

} // namespace ke
