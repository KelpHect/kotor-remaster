#include "script/Corpus.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

namespace ke::script {

namespace {

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("cannot open " + path.string());
    }
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return bytes;
}

std::string escapeJson(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
                out += buffer;
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

std::string number(double value)
{
    if (std::isfinite(value) && value == static_cast<double>(static_cast<std::int64_t>(value))) {
        return std::to_string(static_cast<std::int64_t>(value));
    }
    std::ostringstream stream;
    stream.setf(std::ios::fmtflags(0), std::ios::floatfield);
    stream << std::setprecision(17) << value;
    return stream.str();
}

std::string valueJson(const Value& value)
{
    switch (value.type) {
    case ValueType::Int:
    case ValueType::Object:
    case ValueType::Engine:
        return std::to_string(value.integer);
    case ValueType::Float:
        return number(value.real);
    case ValueType::String:
        return "\"" + escapeJson(value.text) + "\"";
    }
    return "null";
}

std::string hex(std::int32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << static_cast<std::uint32_t>(value);
    return stream.str();
}

} // namespace

CorpusRun runNcsCorpus(const std::filesystem::path& directory, bool functional,
                       std::uint64_t budget, std::size_t limit, bool trace,
                       bool selfAware, bool drain)
{
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".ncs") {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    if (limit != 0 && paths.size() > limit) {
        paths.resize(limit);
    }

    CorpusRun run;
    run.files.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        CorpusFileResult result;
        result.file = path.filename().string();
        try {
            const std::vector<std::uint8_t> bytes = readFile(path);
            const std::vector<Instruction> instructions = decodeNcs(bytes);
            result.instructionCount = instructions.size();
            result.invalidJumps = validateControlFlow(instructions);
            run.invalidJumpTargets += result.invalidJumps.size();

            std::unique_ptr<World> world;
            std::unique_ptr<ActionRuntime> runtime;
            ActionSink* sink = nullptr;
            if (functional) {
                world = std::make_unique<World>();
                runtime = std::make_unique<ActionRuntime>(world.get());
                runtime->setSelfAware(selfAware);
                runtime->setTraceEvents(drain);
                sink = runtime.get();
            }
            Vm vm(instructions, sink, budget, {202}, trace);
            result.vm = vm.run();
            if (trace) {
                result.trace = vm.traceOffsets();
            }
            if (runtime != nullptr) {
                const auto mergeActions = [&]() {
                    for (const std::uint16_t id : runtime->observedActions()) {
                        result.executedActions.insert(id);
                    }
                    runtime->clearTrace();
                    world->events.clear();
                };
                mergeActions();
                if (drain) {
                    const auto runFunctor = [&](const Functor& functor,
                                                std::uint32_t target) {
                        runtime->setContext(functor.context);
                        if (target != 0 && target != kObjectInvalid) {
                            runtime->setContextObject("self", target);
                            runtime->setContextObject("speaker", target);
                        }
                        Vm functorVm(functor.instructions, runtime.get(), 200000);
                        functorVm.setEntry(functor.ip, functor.stack, functor.bp);
                        functorVm.run();
                    };
                    for (int tick = 0; tick < 64; ++tick) {
                        bool ran = false;
                        for (const Timer& timer : world->advance(1.0)) {
                            if (timer.functor < 0) {
                                continue;
                            }
                            const std::shared_ptr<Functor> functor =
                                world->functor(timer.functor);
                            if (functor != nullptr) {
                                runFunctor(*functor, timer.target);
                                world->functors.erase(timer.functor);
                                mergeActions();
                                ran = true;
                            }
                        }
                        std::deque<QueuedAction> remaining;
                        while (!world->actionQueue.empty()) {
                            QueuedAction action =
                                std::move(world->actionQueue.front());
                            world->actionQueue.pop_front();
                            if (action.functor < 0) {
                                remaining.push_back(std::move(action));
                                continue;
                            }
                            const std::shared_ptr<Functor> functor =
                                world->functor(action.functor);
                            if (functor != nullptr) {
                                runFunctor(*functor, action.target);
                                world->functors.erase(action.functor);
                                mergeActions();
                                ran = true;
                            }
                        }
                        world->actionQueue.swap(remaining);
                        if (!ran) {
                            break;
                        }
                    }
                }
                result.runtime = runtime->summary();
            }
        } catch (const NcsError& error) {
            result.decodeError = true;
            result.vm.status = VmStatus::Error;
            result.vm.error = error.what();
            ++run.decodeErrors;
        } catch (const std::exception& error) {
            result.decodeError = true;
            result.vm.status = VmStatus::Error;
            result.vm.error = error.what();
            ++run.decodeErrors;
        }
        run.totalSteps += result.vm.steps;
        for (const auto& [id, count] : result.vm.actions) {
            run.totalActionCalls += count;
        }
        run.maxStack = std::max(run.maxStack, result.vm.maxStack);
        switch (result.vm.status) {
        case VmStatus::Returned: ++run.returned; break;
        case VmStatus::Blocked: ++run.blocked; break;
        case VmStatus::Budget: ++run.budget; break;
        case VmStatus::Error:
            if (!result.decodeError) {
                ++run.vmErrors;
            }
            break;
        case VmStatus::Running: break;
        }
        run.files.push_back(std::move(result));
    }
    return run;
}

void writeCorpusJsonl(const std::filesystem::path& path, const CorpusRun& run)
{
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write " + path.string());
    }
    for (const CorpusFileResult& result : run.files) {
        out << "{\"file\":\"" << escapeJson(result.file) << "\""
            << ",\"instruction_count\":" << result.instructionCount
            << ",\"status\":\"" << (result.decodeError ? "decode_error" : statusName(result.vm.status))
            << "\""
            << ",\"steps\":" << result.vm.steps
            << ",\"max_sp\":" << result.vm.maxStack
            << ",\"action_calls\":";
        std::uint64_t calls = 0;
        for (const auto& [id, count] : result.vm.actions) {
            calls += count;
        }
        out << calls << ",\"actions\":{";
        bool first = true;
        for (const auto& [id, count] : result.vm.actions) {
            if (!first) {
                out << ",";
            }
            first = false;
            out << "\"" << id << "\":" << count;
        }
        out << "},\"executed_actions\":[";
        bool firstAction = true;
        for (const std::uint16_t id : result.executedActions) {
            if (!firstAction) {
                out << ",";
            }
            firstAction = false;
            out << id;
        }
        out << "],\"notes\":{";        first = true;
        for (const auto& [name, count] : result.vm.notes) {
            if (!first) {
                out << ",";
            }
            first = false;
            out << "\"" << escapeJson(name) << "\":" << count;
        }
        out << "},\"error\":";
        if (result.vm.error.empty()) {
            out << "null";
        } else {
            out << "\"" << escapeJson(result.vm.error) << "\"";
        }
        out << ",\"error_ip\":";
        if (result.vm.errorIp < 0) {
            out << "null";
        } else {
            out << "\"" << hex(result.vm.errorIp) << "\"";
        }
        out << ",\"blocked_action\":";
        if (result.vm.hasBlockedAction) {
            out << result.vm.blockedAction;
        } else {
            out << "null";
        }
        out << ",\"top\":";
        if (result.vm.top.has_value()) {
            out << "{\"type\":\"" << result.vm.top->typeName() << "\",\"value\":"
                << valueJson(*result.vm.top) << "}";
        } else {
            out << "null";
        }
        out << ",\"invalid_jump_targets\":[";
        first = true;
        for (const InvalidJump& jump : result.invalidJumps) {
            if (!first) {
                out << ",";
            }
            first = false;
            out << "{\"kind\":\"" << jump.kind << "\",\"at\":\"" << hex(static_cast<std::int32_t>(jump.at))
                << "\",\"target\":\"" << hex(jump.target) << "\"}";
        }
        out << "]";
        if (!result.trace.empty()) {
            out << ",\"trace\":[";
            for (std::size_t i = 0; i < result.trace.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                out << result.trace[i];
            }
            out << "]";
        }
        if (result.runtime.has_value()) {
            const RuntimeSummary& summary = *result.runtime;
            out << ",\"runtime\":{\"calls\":" << summary.calls
                << ",\"unique_actions\":" << summary.uniqueActions << ",\"specific\":[";
            for (std::size_t i = 0; i < summary.specific.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                out << "\"" << escapeJson(summary.specific[i]) << "\"";
            }
            out << "],\"generic\":[";
            for (std::size_t i = 0; i < summary.generic.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                out << "\"" << escapeJson(summary.generic[i]) << "\"";
            }
            out << "],\"default\":[";
            for (std::size_t i = 0; i < summary.defaulted.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                out << "\"" << escapeJson(summary.defaulted[i]) << "\"";
            }
            out << "],\"timers\":" << summary.timers
                << ",\"queued_actions\":" << summary.queuedActions << ",\"world\":{";
            bool firstWorld = true;
            for (const auto& [name, value] : summary.world) {
                if (!firstWorld) {
                    out << ",";
                }
                firstWorld = false;
                out << "\"" << escapeJson(name) << "\":" << number(value);
            }
            out << "}}";
        }
        out << "}\n";
    }
}

} // namespace ke::script
