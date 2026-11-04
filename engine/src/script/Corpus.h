#pragma once

#include "script/Vm.h"
#include "script/ActionRuntime.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ke::script {

struct CorpusFileResult {
    std::string file;
    std::size_t instructionCount = 0;
    VmResult vm;
    std::vector<InvalidJump> invalidJumps;
    bool decodeError = false;
    std::optional<RuntimeSummary> runtime;
    std::vector<std::uint32_t> trace;
    std::set<std::uint16_t> executedActions;
};

struct CorpusRun {
    std::vector<CorpusFileResult> files;
    std::uint64_t totalSteps = 0;
    std::uint64_t totalActionCalls = 0;
    std::size_t decodeErrors = 0;
    std::size_t vmErrors = 0;
    std::size_t blocked = 0;
    std::size_t budget = 0;
    std::size_t returned = 0;
    std::size_t maxStack = 0;
    std::size_t invalidJumpTargets = 0;
};

// Decode and execute every *.ncs in a directory (sorted by name). This is the
// verification path; the runtime itself never opens this directory.
CorpusRun runNcsCorpus(const std::filesystem::path& directory, bool functional,
                       std::uint64_t budget, std::size_t limit = 0, bool trace = false,
                       bool selfAware = false, bool drain = false);

// Write one JSON object per line, matching the Python oracle field names.
void writeCorpusJsonl(const std::filesystem::path& path, const CorpusRun& run);

} // namespace ke::script
